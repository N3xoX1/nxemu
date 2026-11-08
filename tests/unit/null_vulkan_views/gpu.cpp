// SPDX-License-Identifier: GPL-2.0-or-later
// Manual headless integration test. Uses the real Vulkan runtime and texture cache.
#include "fixture.h"
#include "nxemu-video/video_settings.h"
#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/control/channel_state.h"
#include "yuzu_video_core/dma_pusher.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/engines/kepler_memory.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/engines/sw_blitter/blitter.h"
#include "yuzu_video_core/renderer_vulkan/blit_image.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/texture_cache/texture_cache_base.h"
#include "yuzu_video_core/textures/decoders.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "yuzu_video_core/renderer_vulkan/null_image.h"

IModuleSettings* g_settings{};
IModuleNotification* g_notify{};

using Fermi = Tegra::Engines::Fermi2D;
void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
std::atomic<unsigned> validation_errors{};
VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++validation_errors;
        std::fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
    }
    return VK_FALSE;
}
int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: %s /path/to/Vulkan/library\n", argv[0]);
        return 2;
    }
    std::fill(std::begin(Common::Log::g_classLevel), std::end(Common::Log::g_classLevel),
              LogLevel::Warning);
    Common::DynamicLibrary library{argv[1]};
    Require(library.IsOpen(), "Vulkan library opens");
    Vulkan::vk::InstanceDispatch dispatch;
#ifdef __APPLE__
    constexpr auto platform = Core::Frontend::WindowSystemType::Cocoa;
#elif defined(_WIN32)
    constexpr auto platform = Core::Frontend::WindowSystemType::Windows;
#else
    constexpr auto platform = Core::Frontend::WindowSystemType::Wayland;
#endif
    auto instance = Vulkan::CreateInstance(library, dispatch, VK_API_VERSION_1_1,
        platform, true);
    VkDebugUtilsMessengerEXT messenger{};
    const VkDebugUtilsMessengerCreateInfoEXT messenger_ci{
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = Validation};
    Require(dispatch.vkCreateDebugUtilsMessengerEXT(*instance, &messenger_ci, nullptr, &messenger) == VK_SUCCESS,
            "validation messenger created");
    const auto devices = instance.EnumeratePhysicalDevices();
    Require(!devices.empty(), "physical device available");
    {
    Vulkan::Device device{*instance, Vulkan::vk::PhysicalDevice{devices[0], dispatch}, nullptr,
                          dispatch};
    if (device.HasNullDescriptor()) {
        std::puts("SKIP: device uses native null descriptors");
        return 77;
    }
    Vulkan::StateTracker state;
    Vulkan::Scheduler scheduler{device, state};
    Vulkan::MemoryAllocator allocator{device};
    Vulkan::StagingBufferPool staging{device, allocator, scheduler};
    Vulkan::DescriptorPool descriptors{device, scheduler};
    Vulkan::ComputePassDescriptorQueue descriptor_queue{device, scheduler};
    Vulkan::BlitImageHelper blitter{device, scheduler, state, descriptors};
    Vulkan::RenderPassCache render_passes{device};
    Vulkan::TextureCacheRuntime runtime{device, scheduler, allocator, staging,
                                        blitter, render_passes, descriptors, descriptor_queue};
    {
        Vulkan::ImageView view{runtime, VideoCommon::NullImageViewParams{}};
        for (u32 i = 0; i < Shader::NUM_TEXTURE_TYPES; ++i) {
            const auto type = static_cast<Shader::TextureType>(i);
            if (type == Shader::TextureType::Buffer) continue;
            Require(view.Handle(type) != VK_NULL_HANDLE, "fallback view exists");
        }
        for (u32 i = 0; i < Shader::NUM_TEXTURE_TYPES; ++i) {
            const auto type = static_cast<Shader::TextureType>(i);
            if (type == Shader::TextureType::Buffer) continue;
            for (auto format : {Shader::ImageFormat::Typeless, Shader::ImageFormat::R8_UINT,
                                 Shader::ImageFormat::R8_SINT, Shader::ImageFormat::R16_UINT,
                                 Shader::ImageFormat::R16_SINT, Shader::ImageFormat::R32_UINT,
                                 Shader::ImageFormat::R32G32_UINT, Shader::ImageFormat::R32G32B32A32_UINT}) {
                const auto handle = view.StorageView(type, format);
                Require(handle != VK_NULL_HANDLE, "typed null storage view exists");
                Require(view.StorageView(type, format) == handle, "storage view is reused");
            }
        }
        scheduler.Finish();
        Vulkan::ImageView moved{std::move(view)};
        Vulkan::ImageView assigned{runtime, VideoCommon::NullImageViewParams{}};
        scheduler.Finish();
        assigned = std::move(moved);
        scheduler.Finish();
    }
    // Verify every layer of each sampled backing image is cleared to zero.
    for (size_t index = 0; index < Vulkan::NullImage::Count; ++index) {
        auto image = allocator.CreateImage(Vulkan::NullImage::ImageInfo(index));
        const size_t byte_count = Vulkan::NullImage::Range(index).layerCount * 4;
        auto download = staging.Request(byte_count, Vulkan::MemoryUsage::Download);
        std::fill(download.mapped_span.begin(), download.mapped_span.end(), 0xcd);
        const VkImage handle = *image;
        scheduler.Record([handle, index, download](Vulkan::vk::CommandBuffer cmd) {
            Vulkan::NullImage::Initialize(cmd, std::array{handle}, std::array{index});
            const VkMemoryBarrier readable{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, readable);
            const VkBufferImageCopy copy{.bufferOffset = download.offset,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                                    Vulkan::NullImage::Range(index).layerCount},
                .imageExtent = {1, 1, 1}};
            cmd.CopyImageToBuffer(handle, VK_IMAGE_LAYOUT_GENERAL, download.buffer, copy);
        });
        scheduler.Finish();
        Require(std::all_of(download.mapped_span.begin(), download.mapped_span.begin() + byte_count,
                            [](u8 value) { return value == 0; }), "all null image layers are zero");
    }
    }
    dispatch.vkDestroyDebugUtilsMessengerEXT(*instance, messenger, nullptr);
    Require(validation_errors == 0, "no Vulkan validation errors");
    std::puts("Null Vulkan views created and initialized successfully");
}
