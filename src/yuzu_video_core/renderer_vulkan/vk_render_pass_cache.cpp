// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <unordered_map>

#include <boost/container/static_vector.hpp>

#include "yuzu_video_core/renderer_vulkan/maxwell_to_vk.h"
#include "yuzu_video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {
using VideoCore::Surface::PixelFormat;

// Lets a draw read the color attachments written by earlier draws of the subpass
constexpr VkSubpassDependency COLOR_READ_DEPENDENCY{
    .srcSubpass = 0,
    .dstSubpass = 0,
    .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
    .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
};

VkAttachmentDescription AttachmentDescription(const Device& device, PixelFormat format,
                                              VkSampleCountFlagBits samples) {
    using MaxwellToVK::SurfaceFormat;
    return {
        .flags = {},
        .format = SurfaceFormat(device, FormatType::Optimal, true, format).format,
        .samples = samples,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
}
} // Anonymous namespace

RenderPassCache::RenderPassCache(const Device& device_) : device{&device_} {}

VkRenderPass RenderPassCache::Get(const RenderPassKey& key) {
    std::scoped_lock lock{mutex};
    const auto [pair, is_new] = cache.try_emplace(key);
    if (!is_new) {
        return *pair->second;
    }
    boost::container::static_vector<VkAttachmentDescription, 9> descriptions;
    std::array<VkAttachmentReference, 8> references{};
    u32 num_attachments{};
    u32 num_colors{};
    for (size_t index = 0; index < key.color_formats.size(); ++index) {
        const PixelFormat format{key.color_formats[index]};
        const bool is_valid{format != PixelFormat::Invalid};
        references[index] = VkAttachmentReference{
            .attachment = is_valid ? num_colors : VK_ATTACHMENT_UNUSED,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        };
        if (is_valid) {
            auto description = AttachmentDescription(*device, format, key.samples);
            if ((key.color_scratch_mask & (1U << index)) != 0) {
                description.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                description.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                description.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            }
            descriptions.push_back(description);
            num_attachments = static_cast<u32>(index + 1);
            ++num_colors;
        }
    }
    const bool has_depth{key.depth_format != PixelFormat::Invalid};
    VkAttachmentReference depth_reference{};
    if (key.depth_format != PixelFormat::Invalid) {
        depth_reference = VkAttachmentReference{
            .attachment = num_colors,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        };
        descriptions.push_back(AttachmentDescription(*device, key.depth_format, key.samples));
    }
    // Every color attachment is also an input attachment of the same index, so that any
    // pipeline may read the pixel it shades. Render pass compatibility includes them.
    const bool reads_colors{device->SupportsColorAttachmentFetch() && num_attachments != 0};
    const VkSubpassDescription subpass{
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = reads_colors ? num_attachments : 0,
        .pInputAttachments = reads_colors ? references.data() : nullptr,
        .colorAttachmentCount = num_attachments,
        .pColorAttachments = references.data(),
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = has_depth ? &depth_reference : nullptr,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };
    pair->second = device->GetLogical().CreateRenderPass({
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = static_cast<u32>(descriptions.size()),
        .pAttachments = descriptions.empty() ? nullptr : descriptions.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = reads_colors ? 1U : 0U,
        .pDependencies = reads_colors ? &COLOR_READ_DEPENDENCY : nullptr,
    });
    return *pair->second;
}

} // namespace Vulkan
