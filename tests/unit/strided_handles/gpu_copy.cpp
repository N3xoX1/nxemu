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
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
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

void ChainedCopies(Vulkan::TextureCacheRuntime &runtime,
                   Vulkan::StagingBufferPool &staging,
                   Vulkan::Scheduler &scheduler) {
  using namespace VideoCommon;
  using VideoCore::Surface::PixelFormat;
  ImageInfo info;
  info.type = ImageType::e2D;
  info.format = PixelFormat::A8B8G8R8_UNORM;
  info.size = {32, 8, 1};
  Vulkan::Image a{runtime, info, 0x10000, 0x10000};
  info.format = PixelFormat::R16_UNORM;
  info.size.width = 64;
  Vulkan::Image b{runtime, info, 0x20000, 0x20000};
  info.format = PixelFormat::R16G16B16A16_UNORM;
  info.size.width = 16;
  Vulkan::Image c{runtime, info, 0x30000, 0x30000};
  runtime.TransitionImageLayout(b);
  runtime.TransitionImageLayout(c);
  std::vector<u8> bytes(1024);
  const BufferImageCopy region{.buffer_size = bytes.size(),
                               .image_extent = a.info.size};
  const ImageCopy ab{.extent = b.info.size}, bc{.extent = c.info.size},
      ca{.extent = a.info.size};
  for (unsigned iteration = 0; iteration < 64; ++iteration) {
    for (size_t i = 0; i < bytes.size(); ++i)
      bytes[i] = u8(i * 37 + iteration * 13);
    const auto upload =
        staging.Request(bytes.size(), Vulkan::MemoryUsage::Upload);
    std::copy(bytes.begin(), bytes.end(), upload.mapped_span.begin());
    a.UploadMemory(upload, std::span{&region, 1});
    // Every copy reuses the same scratch buffer. The next copy immediately
    // consumes the previous destination, and the last overwrites the first
    // source.
    runtime.CopyImage(b, a, std::span{&ab, 1});
    runtime.CopyImage(c, b, std::span{&bc, 1});
    runtime.CopyImage(a, c, std::span{&ca, 1});
  }
  const auto output =
      staging.Request(bytes.size(), Vulkan::MemoryUsage::Download);
  a.DownloadMemory(output, std::span{&region, 1});
  scheduler.Finish();
  Require(
      std::equal(bytes.begin(), bytes.end(), output.mapped_span.begin()),
      "chained reinterpretations preserve bytes and scratch reuse ordering");
}

void ReinterpretRegions(Vulkan::TextureCacheRuntime &runtime,
                        Vulkan::StagingBufferPool &staging,
                        Vulkan::Scheduler &scheduler, bool check_empty) {
  using namespace VideoCommon;
  using VideoCore::Surface::PixelFormat;
  // Exercise both aspect directions through ReinterpretImage's other caller,
  // which does not need a byte-size change.
  for (bool depth_source : {false, true}) {
    ImageInfo src_info;
    src_info.type = ImageType::e2D;
    src_info.size = {32, 64, 1};
    src_info.format =
        depth_source ? PixelFormat::D32_FLOAT : PixelFormat::R32_FLOAT;
    auto dst_info = src_info;
    dst_info.format =
        depth_source ? PixelFormat::R32_FLOAT : PixelFormat::D32_FLOAT;
    Vulkan::Image src{runtime, src_info, 0x10000, 0x10000};
    Vulkan::Image dst{runtime, dst_info, 0x20000, 0x20000};
    std::vector<u32> values(32 * 64),
        blank(values.size(), std::bit_cast<u32>(0.25f));
    auto expected = blank;
    for (size_t i = 0; i < values.size(); ++i)
      values[i] = std::bit_cast<u32>(float(i % 17) / 16);
    const BufferImageCopy full{.buffer_size = values.size() * 4,
                               .image_extent = src_info.size};
    const auto upload = [&](Vulkan::Image &img, const std::vector<u32> &data) {
      const auto map =
          staging.Request(data.size() * 4, Vulkan::MemoryUsage::Upload);
      std::memcpy(map.mapped_span.data(), data.data(), data.size() * 4);
      img.UploadMemory(map, std::span{&full, 1});
    };
    upload(src, values);
    upload(dst, blank);
    std::vector<ImageCopy> copies;
    for (s32 row = 0; row < 17; ++row) {
      copies.push_back({.src_offset = {0, row, 0},
                        .dst_offset = {0, 63 - row, 0},
                        .extent = {32, 1, 1}});
      std::copy_n(values.begin() + row * 32, 32,
                  expected.begin() + (63 - row) * 32);
    }
    // Keep this regression check outside benchmark mode so that the same
    // benchmark can run against the old implementation, which did not handle
    // an empty span.
    if (check_empty)
      runtime.ReinterpretImage(dst, src, {});
    runtime.ReinterpretImage(dst, src, copies);
    const auto output =
        staging.Request(values.size() * 4, Vulkan::MemoryUsage::Download);
    dst.DownloadMemory(output, std::span{&full, 1});
    scheduler.Finish();
    Require(std::memcmp(expected.data(), output.mapped_span.data(),
                        expected.size() * 4) == 0,
            "17-region depth/color reinterpretations preserve copied and "
            "untouched bytes");
  }
}

void Benchmark(Vulkan::TextureCacheRuntime &runtime,
               Vulkan::StagingBufferPool &staging, Vulkan::Scheduler &scheduler,
               const Vulkan::Device &device) {
  using namespace VideoCommon;
  using VideoCore::Surface::PixelFormat;
  const auto &logical = device.GetLogical();
  const auto &dispatch = device.GetDispatchLoader();
  const auto write_timestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
      dispatch.vkGetDeviceProcAddr(*logical, "vkCmdWriteTimestamp"));
  const auto reset_queries = reinterpret_cast<PFN_vkCmdResetQueryPool>(
      dispatch.vkGetDeviceProcAddr(*logical, "vkCmdResetQueryPool"));
  Require(write_timestamp && reset_queries, "timestamp commands available");
  const auto properties = device.GetPhysical().GetProperties();
  const auto families = device.GetPhysical().GetQueueFamilyProperties();
  const u32 valid_bits =
      families[device.GetGraphicsFamily()].timestampValidBits;
  Require(valid_bits != 0, "graphics queue supports timestamps");
  const u64 timestamp_mask =
      valid_bits == 64 ? ~u64{} : (u64{1} << valid_bits) - 1;
  std::printf("device=%s\nrow_bytes,height,src_bpp,dst_bpp,cpu_record_us,"
              "completed_us,gpu_timestamp_us\n",
              properties.deviceName);
  for (auto dimensions : {std::array{128U, 16U}, std::array{4096U, 512U},
                          std::array{7680U, 1080U}}) {
    for (auto bpp :
         {std::array{4U, 2U}, std::array{2U, 4U}, std::array{4U, 4U}}) {
      const auto [row_bytes, height] = dimensions;
      const auto [src_bpp, dst_bpp] = bpp;
      ImageInfo src_info;
      src_info.type = ImageType::e2D;
      src_info.format =
          src_bpp == 4 ? PixelFormat::A8B8G8R8_UNORM : PixelFormat::R16_UNORM;
      src_info.size = {row_bytes / src_bpp, height, 1};
      auto dst_info = src_info;
      dst_info.format =
          dst_bpp == 4 ? PixelFormat::A8B8G8R8_UNORM : PixelFormat::R16_UNORM;
      dst_info.size.width = row_bytes / dst_bpp;
      Vulkan::Image src{runtime, src_info, 0x10000, 0x10000};
      Vulkan::Image dst{runtime, dst_info, 0x20000, 0x20000};
      const size_t size = size_t{row_bytes} * height;
      std::vector<u8> input(size);
      for (size_t i = 0; i < size; ++i)
        input[i] = u8(i * 37 + i / 97);
      const auto upload = staging.Request(size, Vulkan::MemoryUsage::Upload);
      std::copy(input.begin(), input.end(), upload.mapped_span.begin());
      const BufferImageCopy in{.buffer_size = size,
                               .image_extent = src_info.size};
      src.UploadMemory(upload, std::span{&in, 1});
      runtime.TransitionImageLayout(dst);
      const ImageCopy copy{.extent = dst_info.size};
      for (unsigned i = 0; i < 32; ++i)
        runtime.CopyImage(dst, src, std::span{&copy, 1});
      scheduler.Finish();
      auto queries = logical.CreateQueryPool(
          {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
           .queryType = VK_QUERY_TYPE_TIMESTAMP,
           .queryCount = 2});
      const VkQueryPool pool = *queries;
      const unsigned iterations = size <= 2048 ? 2000 : 256;
      std::array<double, 5> cpu{}, completed{}, gpu{};
      for (unsigned sample = 0; sample < cpu.size(); ++sample) {
        scheduler.Record([=](Vulkan::vk::CommandBuffer cmdbuf) {
          reset_queries(*cmdbuf, pool, 0, 2);
          write_timestamp(*cmdbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
        });
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < iterations; ++i)
          runtime.CopyImage(dst, src, std::span{&copy, 1});
        const auto recorded = std::chrono::steady_clock::now();
        scheduler.Record([=](Vulkan::vk::CommandBuffer cmdbuf) {
          write_timestamp(*cmdbuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool,
                          1);
        });
        scheduler.Finish();
        const auto finished = std::chrono::steady_clock::now();
        std::array<u64, 2> times{};
        Require(logical.GetQueryResults(
                    pool, 0, 2, sizeof(times), times.data(), sizeof(u64),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                    VK_SUCCESS,
                "timestamp results");
        cpu[sample] =
            std::chrono::duration<double, std::micro>(recorded - start)
                .count() /
            iterations;
        completed[sample] =
            std::chrono::duration<double, std::micro>(finished - start)
                .count() /
            iterations;
        gpu[sample] = double((times[1] - times[0]) & timestamp_mask) *
                      properties.limits.timestampPeriod / (1000.0 * iterations);
      }
      std::sort(cpu.begin(), cpu.end());
      std::sort(completed.begin(), completed.end());
      std::sort(gpu.begin(), gpu.end());
      std::printf("%u,%u,%u,%u,%.3f,%.3f,%.3f\n", row_bytes, height, src_bpp,
                  dst_bpp, cpu[2], completed[2], gpu[2]);
      const auto output = staging.Request(size, Vulkan::MemoryUsage::Download);
      const BufferImageCopy out{.buffer_size = size,
                                .image_extent = dst_info.size};
      dst.DownloadMemory(output, std::span{&out, 1});
      scheduler.Finish();
      Require(
          std::equal(input.begin(), input.end(), output.mapped_span.begin()),
          "benchmark copy preserves every byte");
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    std::fprintf(
        stderr,
        "Usage: %s Vulkan-library [--benchmark|--benchmark-no-validation]\n",
        argv[0]);
    return 2;
  }
  const bool validation =
      argc != 3 || std::string_view{argv[2]} != "--benchmark-no-validation";
  std::fill(std::begin(Common::Log::g_classLevel),
            std::end(Common::Log::g_classLevel), LogLevel::Warning);
  Common::DynamicLibrary library{argv[1]};
  Require(library.IsOpen(), "Vulkan library opens");
  Vulkan::vk::InstanceDispatch dispatch;
  auto instance = Vulkan::CreateInstance(
      library, dispatch, VK_API_VERSION_1_1,
#ifdef _WIN32
      Core::Frontend::WindowSystemType::Windows, validation);
#else
      Core::Frontend::WindowSystemType::Headless, validation);
#endif
  Vulkan::vk::DebugUtilsMessenger messenger;
  if (validation)
    messenger = instance.CreateDebugUtilsMessenger({
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
  ChainedCopies(runtime, staging, scheduler);
  ReinterpretRegions(runtime, staging, scheduler, argc == 2);
  std::fprintf(
      stderr,
      "192 chained copies and both 17-region depth/color cases passed\n");
  if (argc == 3)
    Benchmark(runtime, staging, scheduler, device);
  scheduler.Finish();
  std::fprintf(stderr, "%u cross-format GPU cases, validation errors=%u\n",
               checks, errors.load());
  return errors ? 1 : 0;
}
