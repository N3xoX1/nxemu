// SPDX-License-Identifier: GPL-2.0-or-later
// Manual headless integration test. Uses the real Vulkan runtime and texture
// cache.
#include "../fast_copy/fixture.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/engines/kepler_memory.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/renderer_vulkan/blit_image.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/texture_cache/texture_cache_base.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

IModuleSettings *g_settings{};
IModuleNotification *g_notify{};

void Require(bool ok, const char *message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
std::atomic<unsigned> errors{};
VKAPI_ATTR VkBool32 VKAPI_CALL
Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
           VkDebugUtilsMessageTypeFlagsEXT,
           const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    ++errors;
    std::fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
  }
  return VK_FALSE;
}
int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(stderr, "Usage: %s /path/to/Vulkan/library\n", argv[0]);
    return 2;
  }
  std::fill(std::begin(Common::Log::g_classLevel),
            std::end(Common::Log::g_classLevel), LogLevel::Warning);
  Common::DynamicLibrary library{argv[1]};
  Require(library.IsOpen(), "Vulkan library opens");
  Vulkan::vk::InstanceDispatch dispatch;
  auto instance =
      Vulkan::CreateInstance(library, dispatch, VK_API_VERSION_1_1,
#ifdef _WIN32
                             Core::Frontend::WindowSystemType::Windows, true);
#else
                             Core::Frontend::WindowSystemType::Headless, true);
#endif
  auto messenger = instance.CreateDebugUtilsMessenger({
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
      .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
      .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
      .pfnUserCallback = Validation,
  });
  const auto devices = instance.EnumeratePhysicalDevices();
  Require(!devices.empty(), "physical device available");
  Vulkan::Device device{*instance,
                        Vulkan::vk::PhysicalDevice{devices[0], dispatch},
                        nullptr, dispatch};
  CopyFixture memory;
  Tegra::Control::ChannelState channel{0};
  channel.memory_manager =
      std::shared_ptr<Tegra::MemoryManager>(&memory.gpu, [](auto *) {});
  channel.maxwell_3d = std::make_unique<Tegra::Engines::Maxwell3D>(memory.gpu);
  channel.kepler_compute =
      std::make_unique<Tegra::Engines::KeplerCompute>(memory.gpu);
  Vulkan::StateTracker state;
  state.SetupTables(channel);
  state.ChangeChannel(channel);
  Vulkan::Scheduler scheduler{device, state};
  Vulkan::MemoryAllocator allocator{device};
  Vulkan::StagingBufferPool staging{device, allocator, scheduler};
  Vulkan::DescriptorPool descriptors{device, scheduler};
  Vulkan::ComputePassDescriptorQueue descriptor_queue{device, scheduler};
  Vulkan::BlitImageHelper blitter{device, scheduler, state, descriptors};
  Vulkan::RenderPassCache render_passes{device};
  Vulkan::TextureCacheRuntime runtime{
      device,  scheduler,     allocator,   staging,
      blitter, render_passes, descriptors, descriptor_queue};
  std::fprintf(stderr, "Runtime initialization validation errors=%u\n",
               errors.load());
  descriptor_queue.Acquire(2);
  descriptor_queue.AddBuffer(VK_NULL_HANDLE, 64, 0);
  const auto real_buffer = runtime.GetTemporaryBuffer(128);
  descriptor_queue.AddBuffer(real_buffer, 16, 32);
  const auto entries = descriptor_queue.Entries();
  Require(entries[0].buffer.offset == 0 &&
              entries[0].buffer.range == VK_WHOLE_SIZE,
          "null buffer descriptor has the Vulkan-required offset and range");
  Require(entries[1].buffer.buffer == real_buffer &&
              entries[1].buffer.offset == 16 && entries[1].buffer.range == 32,
          "non-null buffer descriptor keeps its range");
  using namespace VideoCommon;
  using VideoCore::Surface::PixelFormat;
  constexpr std::array formats{PixelFormat::R8_UNORM, PixelFormat::R16_UNORM,
                               PixelFormat::A8B8G8R8_UNORM,
                               PixelFormat::R16G16B16A16_UNORM};
  unsigned checks{};
  for (bool clipped : {false, true}) {
    for (bool scaled : {false, true}) {
      for (bool linear : {false, true}) {
        if (scaled && linear)
          continue;
        for (u32 src_bpp : {1U, 2U, 4U, 8U})
          for (u32 dst_bpp : {1U, 2U, 4U, 8U}) {
            if (clipped && src_bpp == dst_bpp)
              continue;
            for (s32 mip : {0, 1}) {
              ImageInfo src_info;
              src_info.format = formats[std::countr_zero(src_bpp)];
              src_info.type = linear ? ImageType::Linear : ImageType::e2D;
              src_info.size = {128 / src_bpp, 16, 1};
              src_info.resources = {2, 3};
              if (linear)
                src_info.pitch = 128;
              src_info.layer_stride = 32768;
              auto dst_info = src_info;
              dst_info.format = formats[std::countr_zero(dst_bpp)];
              dst_info.size.width = 128 / dst_bpp;
              Vulkan::Image src{runtime, src_info, 0x10000, 0x10000};
              Vulkan::Image dst{runtime, dst_info, 0x20000, 0x20000};
              runtime.TransitionImageLayout(src);
              runtime.TransitionImageLayout(dst);
              const u32 row_bytes = 128 >> mip;
              const u32 height = 16 >> mip;
              const size_t size = size_t(row_bytes) * height * 3;
              std::vector<u8> input(size), expected(size, 0xA5);
              for (size_t i = 0; i < size; ++i)
                input[i] = u8(i * 37 + i / 97);
              const auto upload = [&](Vulkan::Image &img,
                                      const std::vector<u8> &bytes, u32 bpp) {
                const auto map =
                    staging.Request(size, Vulkan::MemoryUsage::Upload);
                std::copy(bytes.begin(), bytes.end(), map.mapped_span.begin());
                const BufferImageCopy region{
                    .buffer_size = size,
                    .image_subresource = {mip, 0, 3},
                    .image_extent = {row_bytes / bpp, height, 1}};
                img.UploadMemory(map, std::span{&region, 1});
              };
              upload(src, input, src_bpp);
              upload(dst, expected, dst_bpp);
              // Model a rescaled host image: native metadata is half the
              // allocated dimensions, while Handle() still names the full-size
              // host image.
              if (scaled) {
                videoSettings.resolution_info.up_scale = 2;
                src.info.size.width /= 2;
                src.info.size.height /= 2;
                dst.info.size.width /= 2;
                dst.info.size.height /= 2;
                src.flags |= ImageFlagBits::Rescaled;
                dst.flags |= ImageFlagBits::Rescaled;
              }
              // Two disjoint regions, different source/destination layers and
              // offsets.
              const u32 byte_x = 16;
              const u32 byte_width =
                  clipped ? row_bytes - byte_x : row_bytes - 32;
              std::vector<ImageCopy> regions(2);
              for (unsigned r = 0; r < 2; ++r) {
                const u32 rows = clipped ? height - r - 1 : 2;
                regions[r] = {
                    .src_subresource = {mip, s32(r), 1},
                    .dst_subresource = {mip, s32(2 - r), 1},
                    .src_offset = {s32(byte_x / src_bpp), s32(r), 0},
                    .dst_offset = {s32(byte_x / dst_bpp), s32(r + 1), 0},
                    .extent = {clipped ? row_bytes / dst_bpp
                                       : byte_width / dst_bpp,
                               clipped ? height : 2, 1}};
                for (u32 y = 0; y < rows; ++y)
                  std::copy_n(
                      input.begin() + (r * height + y + r) * row_bytes + byte_x,
                      byte_width,
                      expected.begin() +
                          ((2 - r) * height + y + r + 1) * row_bytes + byte_x);
              }
              if (src_bpp != dst_bpp) {
                // Invalid/empty regions must neither issue Vulkan work nor
                // discard the valid regions in the same batch.
                auto invalid = regions[0];
                invalid.src_subresource.base_level = -1;
                regions.insert(regions.begin(), invalid);
                invalid.src_subresource.base_level = 32;
                regions.push_back(invalid);
                invalid = regions[1];
                invalid.dst_subresource.base_layer = 3;
                regions.push_back(invalid);
                invalid = regions[1];
                invalid.extent.width = 0;
                regions.push_back(invalid);
                invalid = regions[1];
                invalid.src_offset.x = -1;
                regions.push_back(invalid);
                runtime.CopyImage(dst, src, {});
              }
              runtime.CopyImage(dst, src, regions);
              if (argc == 3 && !linear && !scaled && !clipped && mip == 0 &&
                  src_bpp == 4 && (dst_bpp == 4 || dst_bpp == 2)) {
                scheduler.Finish();
                const auto start = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < 10000; ++i)
                  runtime.CopyImage(dst, src, regions);
                const auto recorded = std::chrono::steady_clock::now();
                scheduler.Finish();
                const auto completed = std::chrono::steady_clock::now();
                std::printf(
                    "%u->%u bytes/texel, two regions/copy: CPU record %.3f us, "
                    "completed batch %.3f us/copy\n",
                    src_bpp, dst_bpp,
                    std::chrono::duration<double, std::micro>(recorded - start)
                            .count() /
                        10000,
                    std::chrono::duration<double, std::micro>(completed - start)
                            .count() /
                        10000);
              }
              src.info = src_info;
              dst.info = dst_info;
              src.flags &= ~ImageFlagBits::Rescaled;
              dst.flags &= ~ImageFlagBits::Rescaled;
              videoSettings.resolution_info.up_scale = 1;
              const auto output =
                  staging.Request(size, Vulkan::MemoryUsage::Download);
              const BufferImageCopy read{
                  .buffer_size = size,
                  .image_subresource = {mip, 0, 3},
                  .image_extent = {row_bytes / dst_bpp, height, 1}};
              dst.DownloadMemory(output, std::span{&read, 1});
              scheduler.Finish();
              if (!std::equal(expected.begin(), expected.end(),
                              output.mapped_span.begin())) {
                std::fprintf(stderr,
                             "FAIL bytes: src=%u dst=%u linear=%u mip=%d\n",
                             src_bpp, dst_bpp, linear, mip);
                return 1;
              }
              ++checks;
            }
          }
      }
    }
  }
  scheduler.Finish();
  std::fprintf(stderr, "%u cross-format GPU cases, validation errors=%u\n",
               checks, errors.load());
  return errors ? 1 : 0;
}
