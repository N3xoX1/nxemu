// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "frontend/framebuffer_layout.h"
#include "yuzu_video_core/host_shaders/vulkan_present_capture_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_vert_spv.h"
#include "yuzu_video_core/renderer_vulkan/present/present_push_constants.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace Vulkan::ScaleCapture
{
// Explicit, opt-in diagnostic capture. Normal rendering never polls the disk,
// allocates readback memory, or waits for capture. Captures are not benchmarks.
inline std::filesystem::path directory;
inline std::filesystem::path prefix;
inline bool active{}, source_written{};
inline const char * CaptureDirectory()
{
    static const char * path = std::getenv("NXEMU_SCALE_CAPTURE");
    return path;
}
inline void Poll(bool applet)
{
    if (applet)
        return;
    active = false;
    const char * path = CaptureDirectory();
    if (!path || !*path)
        return;
    // Consecutive captures expose alternating temporal reconstruction variants.
    // A single random frame cannot establish stability. This remains diagnostic only.
    static unsigned remaining_captures{};
    const auto arm = [&] {
        const auto id = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        prefix = directory / ("capture-" + std::to_string(id));
        source_written = false;
        active = true;
    };
    if (remaining_captures)
    {
        --remaining_captures;
        arm();
        return;
    }
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now < next)
        return;
    next = now + std::chrono::milliseconds(250);
    directory = std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t *>(path)));
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(directory / "capture.request", error);
    static auto previous = std::filesystem::file_time_type::min();
    if (error || stamp == previous)
        return;
    previous = stamp;
    static const unsigned burst = [] {
        const char * count = std::getenv("NXEMU_SCALE_CAPTURE_FRAMES");
        return count ? static_cast<unsigned>(std::clamp(std::atoi(count), 1, 8)) : 1U;
    }();
    remaining_captures = burst - 1;
    arm();
}
inline void Write(const char * suffix, std::span<const u8> bytes)
{
    auto path = prefix;
    path += suffix;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

inline void Source(const Device & device, MemoryAllocator & allocator, Scheduler & scheduler,
                   VkImageView source, u32 reported_width, u32 reported_height,
                   Common::Rectangle<float> crop, const Layout::FramebufferLayout & screen,
                   unsigned downscale_filter, unsigned aa)
{
    if (!active || source_written)
        return;
    source_written = true;
    scheduler.RequestOutsideRenderPassOperationContext();
    auto descriptor_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    auto pool = CreateWrappedDescriptorPool(device, 1, 1);
    std::array<VkDescriptorSetLayout, 1> layouts{*descriptor_layout};
    auto sets = CreateWrappedDescriptorSets(pool, layouts);
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof(PresentPushConstants)};
    auto layout = device.GetLogical().CreatePipelineLayout(
        VkPipelineLayoutCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                   .setLayoutCount = 1,
                                   .pSetLayouts = descriptor_layout.address(),
                                   .pushConstantRangeCount = 1,
                                   .pPushConstantRanges = &range});
    auto sampler = CreateWrappedSampler(device, VK_FILTER_NEAREST);
    VkDescriptorImageInfo info{*sampler, source, VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet descriptor{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet = sets[0],
                                          .dstBinding = 0,
                                          .descriptorCount = 1,
                                          .descriptorType =
                                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                          .pImageInfo = &info};
    device.GetLogical().UpdateDescriptorSets({descriptor}, {});
    auto vert = BuildShader(device, VULKAN_PRESENT_VERT_SPV);
    auto frag = BuildShader(device, VULKAN_PRESENT_CAPTURE_FRAG_SPV);
    auto pass =
        CreateWrappedRenderPass(device, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED);
    auto pipeline = CreateWrappedPipeline(device, pass, layout, std::tie(vert, frag));
    const auto read = [&](u32 width, u32 height, bool size_only) {
        auto image = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {width, height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED});
        auto view = CreateWrappedImageView(device, image, VK_FORMAT_R8G8B8A8_UNORM);
        auto framebuffer = CreateWrappedFramebuffer(device, pass, view, {width, height});
        auto buffer =
            CreateWrappedBuffer(allocator, u64(width) * height * 4, MemoryUsage::Download);
        PresentPushConstants pc{};
        pc.transform = MakeOrthographicTransform(float(width), float(height));
        pc.vertices = {ScreenRectVertex(0, 0, 0, 0), ScreenRectVertex(float(width), 0, 1, 0),
                       ScreenRectVertex(0, float(height), 0, 1),
                       ScreenRectVertex(float(width), float(height), 1, 1)};
        pc.filter_constants[0] = size_only ? 1 : 0;
        scheduler.Record([&](vk::CommandBuffer cmd) {
            const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, barrier, nullptr,
                                nullptr);
            BeginRenderPass(cmd, *pass, *framebuffer, {width, height});
            cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
            cmd.PushConstants(*layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              pc);
            cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *layout, 0, sets[0], {});
            cmd.Draw(4, 1, 0, 0);
            cmd.EndRenderPass();
            DownloadColorImage(cmd, *image, *buffer, {width, height, 1});
        });
        scheduler.Finish();
        buffer.Invalidate();
        auto mapped = buffer.Mapped();
        return std::vector<u8>(mapped.begin(), mapped.begin() + u64(width) * height * 4);
    };
    const auto size = read(1, 1, true);
    const u32 width = size[0] | u32(size[1]) << 8;
    const u32 height = size[2] | u32(size[3]) << 8;
    if (!width || !height ||
        width > device.GetPhysical().GetProperties().limits.maxImageDimension2D ||
        height > device.GetPhysical().GetProperties().limits.maxImageDimension2D)
    {
        active = false;
        return;
    }
    Write("-source.rgba", read(width, height, false));
    auto metadata = prefix;
    metadata += ".json";
    std::ofstream json(metadata);
    json << "{\n\"source_width\":" << width << ",\n\"source_height\":" << height
         << ",\n\"reported_width\":" << reported_width
         << ",\n\"reported_height\":" << reported_height << ",\n\"crop\":[" << crop.left << ','
         << crop.top << ',' << crop.right << ',' << crop.bottom << "],\n\"screen\":["
         << screen.screen.left << ',' << screen.screen.top << ',' << screen.screen.right << ','
         << screen.screen.bottom << "],\n\"output_width\":" << screen.width
         << ",\n\"output_height\":" << screen.height << ",\n\"downscale_filter\":" << downscale_filter
         << ",\n\"anti_aliasing\":" << aa << "\n}\n";
}
inline void Output(MemoryAllocator & allocator, Scheduler & scheduler, VkImage image, u32 width,
                   u32 height, VkFormat format)
{
    if (!active || !source_written)
        return;
    scheduler.RequestOutsideRenderPassOperationContext();
    auto buffer = CreateWrappedBuffer(allocator, u64(width) * height * 4, MemoryUsage::Download);
    scheduler.Record([&](vk::CommandBuffer cmd) {
        DownloadColorImage(cmd, image, *buffer, {width, height, 1});
    });
    scheduler.Finish();
    buffer.Invalidate();
    auto bytes = buffer.Mapped();
    // Presentation images use BGRA or RGBA8. Store canonical RGBA, preserving alpha.
    std::vector<u8> rgba(bytes.begin(), bytes.begin() + u64(width) * height * 4);
    if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB)
        for (size_t i = 0; i < rgba.size(); i += 4)
            std::swap(rgba[i], rgba[i + 2]);
    Write("-output.rgba", rgba);
    active = false;
}
} // namespace Vulkan::ScaleCapture
