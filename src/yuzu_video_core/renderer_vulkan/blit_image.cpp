// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"

#include "video_settings.h"
#include "yuzu_common/settings.h"
#include "yuzu_video_core/host_shaders/blit_color_float_frag_spv.h"
#include "yuzu_video_core/host_shaders/blit_color_msaa_frag_spv.h"
#include "yuzu_video_core/host_shaders/blit_color_msaa_sint_frag_spv.h"
#include "yuzu_video_core/host_shaders/blit_color_msaa_uint_frag_spv.h"
#include "yuzu_video_core/host_shaders/blit_depth_msaa_frag_spv.h"
#include "yuzu_video_core/host_shaders/blit_depth_stencil_msaa_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_abgr8_to_d24s8_depth_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_abgr8_to_d24s8_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_abgr8_to_s8d24_depth_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_abgr8_to_s8d24_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_abgr8_to_d32f_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_d24s8_to_abgr8_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_d32f_to_abgr8_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_depth_to_float_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_float_to_depth_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_non_msaa_to_msaa_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_non_msaa_to_msaa_sint_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_non_msaa_to_msaa_uint_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_non_msaa_to_msaa_depth_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_non_msaa_to_msaa_depth_stencil_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_packed_depth_to_msaa_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_packed_depth_stencil_to_msaa_frag_spv.h"
#include "yuzu_video_core/host_shaders/convert_s8d24_to_abgr8_frag_spv.h"
#include "yuzu_video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_blit_depth_stencil_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_color_clear_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_color_clear_sint_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_color_clear_uint_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_color_clear_vert_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_depthstencil_clear_frag_spv.h"
#include "yuzu_video_core/renderer_vulkan/blit_image.h"
#include "yuzu_video_core/renderer_vulkan/maxwell_to_vk.h"
#include "yuzu_video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/texture_cache/samples_helper.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

using VideoCommon::ImageViewType;

namespace {
template <typename Key>
size_t FindCachedKey(const std::vector<Key> & keys, const Key & key, size_t & last)
{
    if (last < keys.size() && keys[last] == key)
    {
        return last;
    }
    const auto it = std::ranges::find(keys, key);
    last = static_cast<size_t>(std::distance(keys.begin(), it));
    return last;
}

struct PushConstants {
    std::array<float, 2> tex_scale;
    std::array<float, 2> tex_offset;
};

struct MSAACopyPushConstants {
    std::array<s32, 2> dst_offset;
    std::array<s32, 2> src_offset;
    std::array<s32, 2> scale;
    s32 packed_format{};
};

template <u32 binding>
inline constexpr VkDescriptorSetLayoutBinding TEXTURE_DESCRIPTOR_SET_LAYOUT_BINDING{
    .binding = binding,
    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
    .descriptorCount = 1,
    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    .pImmutableSamplers = nullptr,
};
constexpr std::array TWO_TEXTURES_DESCRIPTOR_SET_LAYOUT_BINDINGS{
    TEXTURE_DESCRIPTOR_SET_LAYOUT_BINDING<0>,
    TEXTURE_DESCRIPTOR_SET_LAYOUT_BINDING<1>,
};
constexpr VkDescriptorSetLayoutCreateInfo ONE_TEXTURE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .bindingCount = 1,
    .pBindings = &TEXTURE_DESCRIPTOR_SET_LAYOUT_BINDING<0>,
};
template <u32 num_textures>
inline constexpr DescriptorBankInfo TEXTURE_DESCRIPTOR_BANK_INFO{
    .uniform_buffers = 0,
    .storage_buffers = 0,
    .texture_buffers = 0,
    .image_buffers = 0,
    .textures = num_textures,
    .images = 0,
    .score = 2,
};
constexpr VkDescriptorSetLayoutCreateInfo TWO_TEXTURES_DESCRIPTOR_SET_LAYOUT_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .bindingCount = static_cast<u32>(TWO_TEXTURES_DESCRIPTOR_SET_LAYOUT_BINDINGS.size()),
    .pBindings = TWO_TEXTURES_DESCRIPTOR_SET_LAYOUT_BINDINGS.data(),
};
template <VkShaderStageFlags stageFlags, size_t size>
inline constexpr VkPushConstantRange PUSH_CONSTANT_RANGE{
    .stageFlags = stageFlags,
    .offset = 0,
    .size = static_cast<u32>(size),
};
constexpr VkPipelineVertexInputStateCreateInfo PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .vertexBindingDescriptionCount = 0,
    .pVertexBindingDescriptions = nullptr,
    .vertexAttributeDescriptionCount = 0,
    .pVertexAttributeDescriptions = nullptr,
};
constexpr VkPipelineInputAssemblyStateCreateInfo PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    .primitiveRestartEnable = VK_FALSE,
};
constexpr VkPipelineViewportStateCreateInfo PIPELINE_VIEWPORT_STATE_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .viewportCount = 1,
    .pViewports = nullptr,
    .scissorCount = 1,
    .pScissors = nullptr,
};
constexpr VkPipelineRasterizationStateCreateInfo PIPELINE_RASTERIZATION_STATE_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .depthClampEnable = VK_FALSE,
    .rasterizerDiscardEnable = VK_FALSE,
    .polygonMode = VK_POLYGON_MODE_FILL,
    .cullMode = VK_CULL_MODE_BACK_BIT,
    .frontFace = VK_FRONT_FACE_CLOCKWISE,
    .depthBiasEnable = VK_FALSE,
    .depthBiasConstantFactor = 0.0f,
    .depthBiasClamp = 0.0f,
    .depthBiasSlopeFactor = 0.0f,
    .lineWidth = 1.0f,
};
constexpr VkPipelineMultisampleStateCreateInfo PIPELINE_MULTISAMPLE_STATE_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    .sampleShadingEnable = VK_FALSE,
    .minSampleShading = 0.0f,
    .pSampleMask = nullptr,
    .alphaToCoverageEnable = VK_FALSE,
    .alphaToOneEnable = VK_FALSE,
};
constexpr std::array DYNAMIC_STATES{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                    VK_DYNAMIC_STATE_BLEND_CONSTANTS};
constexpr VkPipelineDynamicStateCreateInfo PIPELINE_DYNAMIC_STATE_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .dynamicStateCount = static_cast<u32>(DYNAMIC_STATES.size()),
    .pDynamicStates = DYNAMIC_STATES.data(),
};
constexpr VkPipelineColorBlendStateCreateInfo PIPELINE_COLOR_BLEND_STATE_EMPTY_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .logicOpEnable = VK_FALSE,
    .logicOp = VK_LOGIC_OP_CLEAR,
    .attachmentCount = 0,
    .pAttachments = nullptr,
    .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
};
constexpr VkPipelineColorBlendAttachmentState PIPELINE_COLOR_BLEND_ATTACHMENT_STATE{
    .blendEnable = VK_FALSE,
    .srcColorBlendFactor = VK_BLEND_FACTOR_ZERO,
    .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
    .colorBlendOp = VK_BLEND_OP_ADD,
    .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
    .alphaBlendOp = VK_BLEND_OP_ADD,
    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
};
constexpr VkPipelineColorBlendStateCreateInfo PIPELINE_COLOR_BLEND_STATE_GENERIC_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .logicOpEnable = VK_FALSE,
    .logicOp = VK_LOGIC_OP_CLEAR,
    .attachmentCount = 1,
    .pAttachments = &PIPELINE_COLOR_BLEND_ATTACHMENT_STATE,
    .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
};
constexpr VkPipelineDepthStencilStateCreateInfo PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .depthTestEnable = VK_TRUE,
    .depthWriteEnable = VK_TRUE,
    .depthCompareOp = VK_COMPARE_OP_ALWAYS,
    .depthBoundsTestEnable = VK_FALSE,
    .stencilTestEnable = VK_FALSE,
    .front = VkStencilOpState{},
    .back = VkStencilOpState{},
    .minDepthBounds = 0.0f,
    .maxDepthBounds = 0.0f,
};
// The stencil value exported by the fragment shader is only written when the stencil test runs.
constexpr VkStencilOpState STENCIL_EXPORT_OP_STATE{
    .failOp = VK_STENCIL_OP_REPLACE,
    .passOp = VK_STENCIL_OP_REPLACE,
    .depthFailOp = VK_STENCIL_OP_REPLACE,
    .compareOp = VK_COMPARE_OP_ALWAYS,
    .compareMask = 0xFF,
    .writeMask = 0xFF,
    .reference = 0,
};
constexpr VkPipelineDepthStencilStateCreateInfo PIPELINE_DEPTH_STENCIL_EXPORT_STATE_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .depthTestEnable = VK_TRUE,
    .depthWriteEnable = VK_TRUE,
    .depthCompareOp = VK_COMPARE_OP_ALWAYS,
    .depthBoundsTestEnable = VK_FALSE,
    .stencilTestEnable = VK_TRUE,
    .front = STENCIL_EXPORT_OP_STATE,
    .back = STENCIL_EXPORT_OP_STATE,
    .minDepthBounds = 0.0f,
    .maxDepthBounds = 0.0f,
};

template <VkFilter filter>
inline constexpr VkSamplerCreateInfo SAMPLER_CREATE_INFO{
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .pNext = nullptr,
    .flags = 0,
    .magFilter = filter,
    .minFilter = filter,
    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
    .mipLodBias = 0.0f,
    .anisotropyEnable = VK_FALSE,
    .maxAnisotropy = 0.0f,
    .compareEnable = VK_FALSE,
    .compareOp = VK_COMPARE_OP_NEVER,
    .minLod = 0.0f,
    .maxLod = 0.0f,
    .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
    .unnormalizedCoordinates = VK_TRUE,
};

constexpr VkPipelineLayoutCreateInfo PipelineLayoutCreateInfo(
    const VkDescriptorSetLayout* set_layout, vk::Span<VkPushConstantRange> push_constants) {
    return VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = (set_layout != nullptr ? 1u : 0u),
        .pSetLayouts = set_layout,
        .pushConstantRangeCount = push_constants.size(),
        .pPushConstantRanges = push_constants.data(),
    };
}

constexpr VkPipelineShaderStageCreateInfo PipelineShaderStageCreateInfo(VkShaderStageFlagBits stage,
                                                                        VkShaderModule shader) {
    return VkPipelineShaderStageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage = stage,
        .module = shader,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };
}

constexpr std::array<VkPipelineShaderStageCreateInfo, 2> MakeStages(
    VkShaderModule vertex_shader, VkShaderModule fragment_shader) {
    return std::array{
        PipelineShaderStageCreateInfo(VK_SHADER_STAGE_VERTEX_BIT, vertex_shader),
        PipelineShaderStageCreateInfo(VK_SHADER_STAGE_FRAGMENT_BIT, fragment_shader),
    };
}

[[nodiscard]] VkSampleCountFlagBits SampleCountFlag(u32 num_samples) {
    switch (num_samples) {
    case 2:
        return VK_SAMPLE_COUNT_2_BIT;
    case 4:
        return VK_SAMPLE_COUNT_4_BIT;
    case 8:
        return VK_SAMPLE_COUNT_8_BIT;
    case 16:
        return VK_SAMPLE_COUNT_16_BIT;
    default:
        return VK_SAMPLE_COUNT_1_BIT;
    }
}

[[nodiscard]] MSAACopyFormatClass FormatClass(VideoCore::Surface::PixelFormat format) {
    if (!VideoCore::Surface::IsPixelFormatInteger(format)) {
        return MSAACopyFormatClass::Float;
    }
    if (VideoCore::Surface::IsPixelFormatSignedInteger(format)) {
        return MSAACopyFormatClass::SignedInteger;
    }
    return MSAACopyFormatClass::UnsignedInteger;
}

[[nodiscard]] vk::ImageView MakeMSAACopyView(const vk::Device& device, VkImage image,
                                             VkFormat format, u32 base_level, u32 base_layer,
                                             VkImageAspectFlags aspect_mask) {
    return device.CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange{
            .aspectMask = aspect_mask,
            .baseMipLevel = base_level,
            .levelCount = 1,
            .baseArrayLayer = base_layer,
            .layerCount = 1,
        },
    });
}

void UpdateOneTextureDescriptorSet(const Device& device, VkDescriptorSet descriptor_set,
                                   VkSampler sampler, VkImageView image_view) {
    const VkDescriptorImageInfo image_info{
        .sampler = sampler,
        .imageView = image_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkWriteDescriptorSet write_descriptor_set{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    device.GetLogical().UpdateDescriptorSets(write_descriptor_set, nullptr);
}

void UpdateTwoTexturesDescriptorSet(const Device& device, VkDescriptorSet descriptor_set,
                                    VkSampler sampler, VkImageView image_view_0,
                                    VkImageView image_view_1) {
    const VkDescriptorImageInfo image_info_0{
        .sampler = sampler,
        .imageView = image_view_0,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorImageInfo image_info_1{
        .sampler = sampler,
        .imageView = image_view_1,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const std::array write_descriptor_sets{
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = descriptor_set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info_0,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        VkWriteDescriptorSet{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = descriptor_set,
            .dstBinding = 1,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info_1,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
    };
    device.GetLogical().UpdateDescriptorSets(write_descriptor_sets, nullptr);
}

void BindBlitState(vk::CommandBuffer cmdbuf, const Region2D& dst_region) {
    const VkOffset2D offset{
        .x = std::min(dst_region.start.x, dst_region.end.x),
        .y = std::min(dst_region.start.y, dst_region.end.y),
    };
    const VkExtent2D extent{
        .width = static_cast<u32>(std::abs(dst_region.end.x - dst_region.start.x)),
        .height = static_cast<u32>(std::abs(dst_region.end.y - dst_region.start.y)),
    };
    const VkViewport viewport{
        .x = static_cast<float>(offset.x),
        .y = static_cast<float>(offset.y),
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    // TODO: Support scissored blits
    const VkRect2D scissor{
        .offset = offset,
        .extent = extent,
    };
    cmdbuf.SetViewport(0, viewport);
    cmdbuf.SetScissor(0, scissor);
}

void BindBlitState(vk::CommandBuffer cmdbuf, VkPipelineLayout layout, const Region2D& dst_region,
                   const Region2D& src_region, const Extent3D& src_size = {1, 1, 1}) {
    BindBlitState(cmdbuf, dst_region);
    const float scale_x = static_cast<float>(src_region.end.x - src_region.start.x) /
                          static_cast<float>(src_size.width);
    const float scale_y = static_cast<float>(src_region.end.y - src_region.start.y) /
                          static_cast<float>(src_size.height);
    const PushConstants push_constants{
        .tex_scale = {scale_x, scale_y},
        .tex_offset = {static_cast<float>(src_region.start.x) / static_cast<float>(src_size.width),
                       static_cast<float>(src_region.start.y) /
                           static_cast<float>(src_size.height)},
    };
    cmdbuf.PushConstants(layout, VK_SHADER_STAGE_VERTEX_BIT, push_constants);
}

// Makes earlier writes to an image visible to a fragment shader that samples it.
void RecordShaderReadBarrier(Scheduler& scheduler, VkImage image, VkImageAspectFlags aspect_mask) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image, aspect_mask](vk::CommandBuffer cmdbuf) {
        const VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, barrier);
    });
}

VkExtent2D GetConversionExtent(const ImageView& src_image_view) {
    const auto& resolution = videoSettings.resolution_info;
    const bool is_rescaled = src_image_view.IsRescaled();
    u32 width = src_image_view.size.width;
    u32 height = src_image_view.size.height;
    return VkExtent2D{
        .width = is_rescaled ? resolution.ScaleUp(width) : width,
        .height = is_rescaled ? resolution.ScaleUp(height) : height,
    };
}

void SynchronizeBlitSource(vk::CommandBuffer& cmdbuf, VkImage image) {
    // Cached images and sampled descriptors use GENERAL. Make prior writes visible
    // across the source image, including views of nonzero mip levels or layers.
    const VkImageMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        },
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, barrier);
}

void BeginRenderPass(vk::CommandBuffer& cmdbuf, VkRenderPass render_pass,
                     VkFramebuffer framebuffer_handle, VkExtent2D render_area) {
    const VkRenderPassBeginInfo renderpass_bi{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = render_pass,
        .framebuffer = framebuffer_handle,
        .renderArea{
            .offset{},
            .extent = render_area,
        },
        .clearValueCount = 0,
        .pClearValues = nullptr,
    };
    cmdbuf.BeginRenderPass(renderpass_bi, VK_SUBPASS_CONTENTS_INLINE);
}
} // Anonymous namespace

BlitImageHelper::BlitImageHelper(const Device& device_, Scheduler& scheduler_,
                                 StateTracker& state_tracker_, DescriptorPool& descriptor_pool)
    : device{device_}, scheduler{scheduler_}, state_tracker{state_tracker_},
      one_texture_set_layout(device.GetLogical().CreateDescriptorSetLayout(
          ONE_TEXTURE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO)),
      two_textures_set_layout(device.GetLogical().CreateDescriptorSetLayout(
          TWO_TEXTURES_DESCRIPTOR_SET_LAYOUT_CREATE_INFO)),
      one_texture_descriptor_allocator{
          descriptor_pool.Allocator(*one_texture_set_layout, TEXTURE_DESCRIPTOR_BANK_INFO<1>)},
      two_textures_descriptor_allocator{
          descriptor_pool.Allocator(*two_textures_set_layout, TEXTURE_DESCRIPTOR_BANK_INFO<2>)},
      one_texture_pipeline_layout(device.GetLogical().CreatePipelineLayout(PipelineLayoutCreateInfo(
          one_texture_set_layout.address(),
          PUSH_CONSTANT_RANGE<VK_SHADER_STAGE_VERTEX_BIT, sizeof(PushConstants)>))),
      two_textures_pipeline_layout(
          device.GetLogical().CreatePipelineLayout(PipelineLayoutCreateInfo(
              two_textures_set_layout.address(),
              PUSH_CONSTANT_RANGE<VK_SHADER_STAGE_VERTEX_BIT, sizeof(PushConstants)>))),
      clear_color_pipeline_layout(device.GetLogical().CreatePipelineLayout(PipelineLayoutCreateInfo(
          nullptr, PUSH_CONSTANT_RANGE<VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(float) * 4>))),
      msaa_copy_pipeline_layout(device.GetLogical().CreatePipelineLayout(PipelineLayoutCreateInfo(
          one_texture_set_layout.address(),
          PUSH_CONSTANT_RANGE<VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(MSAACopyPushConstants)>))),
      msaa_copy_depth_stencil_pipeline_layout(
          device.GetLogical().CreatePipelineLayout(PipelineLayoutCreateInfo(
              two_textures_set_layout.address(),
              PUSH_CONSTANT_RANGE<VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(MSAACopyPushConstants)>))),
      full_screen_vert(BuildShader(device, FULL_SCREEN_TRIANGLE_VERT_SPV)),
      blit_color_to_color_frag(BuildShader(device, BLIT_COLOR_FLOAT_FRAG_SPV)),
      blit_color_msaa_frag(BuildShader(device, BLIT_COLOR_MSAA_FRAG_SPV)),
      blit_color_msaa_sint_frag(BuildShader(device, BLIT_COLOR_MSAA_SINT_FRAG_SPV)),
      blit_color_msaa_uint_frag(BuildShader(device, BLIT_COLOR_MSAA_UINT_FRAG_SPV)),
      blit_depth_msaa_frag(BuildShader(device, BLIT_DEPTH_MSAA_FRAG_SPV)),
      blit_depth_stencil_msaa_frag(device.IsExtShaderStencilExportSupported()
                                       ? BuildShader(device, BLIT_DEPTH_STENCIL_MSAA_FRAG_SPV)
                                       : vk::ShaderModule{}),
      blit_depth_stencil_frag(BuildShader(device, VULKAN_BLIT_DEPTH_STENCIL_FRAG_SPV)),
      clear_color_vert(BuildShader(device, VULKAN_COLOR_CLEAR_VERT_SPV)),
      clear_color_frag(BuildShader(device, VULKAN_COLOR_CLEAR_FRAG_SPV)),
      clear_stencil_frag(BuildShader(device, VULKAN_DEPTHSTENCIL_CLEAR_FRAG_SPV)),
      convert_depth_to_float_frag(BuildShader(device, CONVERT_DEPTH_TO_FLOAT_FRAG_SPV)),
      convert_float_to_depth_frag(BuildShader(device, CONVERT_FLOAT_TO_DEPTH_FRAG_SPV)),
      // Without shader stencil export these conversions only write the depth.
      convert_abgr8_to_d24s8_frag(BuildShader(
          device, device.IsExtShaderStencilExportSupported()
                      ? std::span<const u32>{CONVERT_ABGR8_TO_D24S8_FRAG_SPV}
                      : std::span<const u32>{CONVERT_ABGR8_TO_D24S8_DEPTH_FRAG_SPV})),
      convert_abgr8_to_s8d24_frag(BuildShader(
          device, device.IsExtShaderStencilExportSupported()
                      ? std::span<const u32>{CONVERT_ABGR8_TO_S8D24_FRAG_SPV}
                      : std::span<const u32>{CONVERT_ABGR8_TO_S8D24_DEPTH_FRAG_SPV})),
      convert_abgr8_to_d32f_frag(BuildShader(device, CONVERT_ABGR8_TO_D32F_FRAG_SPV)),
      convert_d32f_to_abgr8_frag(BuildShader(device, CONVERT_D32F_TO_ABGR8_FRAG_SPV)),
      convert_d24s8_to_abgr8_frag(BuildShader(device, CONVERT_D24S8_TO_ABGR8_FRAG_SPV)),
      convert_s8d24_to_abgr8_frag(BuildShader(device, CONVERT_S8D24_TO_ABGR8_FRAG_SPV)),
      convert_non_msaa_to_msaa_frag(BuildShader(device, CONVERT_NON_MSAA_TO_MSAA_FRAG_SPV)),
      convert_non_msaa_to_msaa_sint_frag(
          BuildShader(device, CONVERT_NON_MSAA_TO_MSAA_SINT_FRAG_SPV)),
      convert_non_msaa_to_msaa_uint_frag(
          BuildShader(device, CONVERT_NON_MSAA_TO_MSAA_UINT_FRAG_SPV)),
      convert_non_msaa_to_msaa_depth_frag(
          BuildShader(device, CONVERT_NON_MSAA_TO_MSAA_DEPTH_FRAG_SPV)),
      convert_non_msaa_to_msaa_depth_stencil_frag(
          device.IsExtShaderStencilExportSupported()
              ? BuildShader(device, CONVERT_NON_MSAA_TO_MSAA_DEPTH_STENCIL_FRAG_SPV)
              : vk::ShaderModule{}),
      convert_packed_depth_to_msaa_frag(
          BuildShader(device, CONVERT_PACKED_DEPTH_TO_MSAA_FRAG_SPV)),
      convert_packed_depth_stencil_to_msaa_frag(
          device.IsExtShaderStencilExportSupported()
              ? BuildShader(device, CONVERT_PACKED_DEPTH_STENCIL_TO_MSAA_FRAG_SPV)
              : vk::ShaderModule{}),
      linear_sampler(device.GetLogical().CreateSampler(SAMPLER_CREATE_INFO<VK_FILTER_LINEAR>)),
      nearest_sampler(device.GetLogical().CreateSampler(SAMPLER_CREATE_INFO<VK_FILTER_NEAREST>)) {}

BlitImageHelper::~BlitImageHelper() = default;

void BlitImageHelper::BlitColor(const Framebuffer* dst_framebuffer, VkImageView src_view,
                                const Region2D& dst_region, const Region2D& src_region,
                                Tegra::Engines::Fermi2D::Filter filter,
                                Tegra::Engines::Fermi2D::Operation operation) {
    const bool is_linear = filter == Tegra::Engines::Fermi2D::Filter::Bilinear;
    const u32 color_scratch_mask = dst_framebuffer->ColorScratchMask(1U);
    const bool is_moltenvk = device.GetDriverID() == VK_DRIVER_ID_MOLTENVK;
    const BlitImagePipelineKey key{
        .renderpass = dst_framebuffer->RenderPassVariant(color_scratch_mask),
        .operation = operation,
        .samples = is_moltenvk ? dst_framebuffer->Samples() : VK_SAMPLE_COUNT_1_BIT,
        .color_attachment_count =
            static_cast<u8>(is_moltenvk ? dst_framebuffer->NumColorAttachments() : 1),
    };
    const VkPipelineLayout layout = *one_texture_pipeline_layout;
    const VkSampler sampler = is_linear ? *linear_sampler : *nearest_sampler;
    const VkPipeline pipeline = FindOrEmplaceColorPipeline(key);
    scheduler.RequestRenderpass(dst_framebuffer, color_scratch_mask);
    scheduler.Record([this, dst_region, src_region, pipeline, layout, sampler,
                      src_view](vk::CommandBuffer cmdbuf) {
        // TODO: Barriers
        const VkDescriptorSet descriptor_set = one_texture_descriptor_allocator.Commit();
        UpdateOneTextureDescriptorSet(device, descriptor_set, sampler, src_view);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptor_set,
                                  nullptr);
        BindBlitState(cmdbuf, layout, dst_region, src_region);
        cmdbuf.Draw(3, 1, 0, 0);
    });
    scheduler.InvalidateState();
}

void BlitImageHelper::BlitColor(const Framebuffer* dst_framebuffer, VkImageView src_image_view,
                                VkImage src_image, VkSampler src_sampler,
                                const Region2D& dst_region, const Region2D& src_region,
                                const Extent3D& src_size) {
    const u32 color_scratch_mask = dst_framebuffer->ColorScratchMask(1U);
    const bool is_moltenvk = device.GetDriverID() == VK_DRIVER_ID_MOLTENVK;
    const BlitImagePipelineKey key{
        .renderpass = dst_framebuffer->RenderPassVariant(color_scratch_mask),
        .operation = Tegra::Engines::Fermi2D::Operation::SrcCopy,
        .samples = is_moltenvk ? dst_framebuffer->Samples() : VK_SAMPLE_COUNT_1_BIT,
        .color_attachment_count =
            static_cast<u8>(is_moltenvk ? dst_framebuffer->NumColorAttachments() : 1),
    };
    const VkPipelineLayout layout = *one_texture_pipeline_layout;
    const VkPipeline pipeline = FindOrEmplaceColorPipeline(key);
    const VkRenderPass render_pass = key.renderpass;
    const VkFramebuffer framebuffer_handle = dst_framebuffer->HandleVariant(color_scratch_mask);
    const VkExtent2D render_area = dst_framebuffer->RenderArea();
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, render_pass, framebuffer_handle, render_area, src_image_view, src_image,
                      src_sampler, dst_region, src_region, src_size, pipeline,
                      layout](vk::CommandBuffer cmdbuf) {
        SynchronizeBlitSource(cmdbuf, src_image);
        BeginRenderPass(cmdbuf, render_pass, framebuffer_handle, render_area);
        const VkDescriptorSet descriptor_set = one_texture_descriptor_allocator.Commit();
        UpdateOneTextureDescriptorSet(device, descriptor_set, src_sampler, src_image_view);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptor_set,
                                  nullptr);
        BindBlitState(cmdbuf, layout, dst_region, src_region, src_size);
        cmdbuf.Draw(3, 1, 0, 0);
        cmdbuf.EndRenderPass();
    });
    scheduler.InvalidateState();
}

void BlitImageHelper::BlitDepthStencil(const Framebuffer* dst_framebuffer,
                                       VkImageView src_depth_view, VkImageView src_stencil_view,
                                       const Region2D& dst_region, const Region2D& src_region,
                                       Tegra::Engines::Fermi2D::Filter filter,
                                       Tegra::Engines::Fermi2D::Operation operation) {
    if (!device.IsExtShaderStencilExportSupported()) {
        return;
    }
    ASSERT(filter == Tegra::Engines::Fermi2D::Filter::Point);
    ASSERT(operation == Tegra::Engines::Fermi2D::Operation::SrcCopy);
    const BlitImagePipelineKey key{
        .renderpass = dst_framebuffer->RenderPass(),
        .operation = operation,
    };
    const VkPipelineLayout layout = *two_textures_pipeline_layout;
    const VkSampler sampler = *nearest_sampler;
    const VkPipeline pipeline = FindOrEmplaceDepthStencilPipeline(key);
    scheduler.RequestRenderpass(dst_framebuffer);
    scheduler.Record([dst_region, src_region, pipeline, layout, sampler, src_depth_view,
                      src_stencil_view, this](vk::CommandBuffer cmdbuf) {
        // TODO: Barriers
        const VkDescriptorSet descriptor_set = two_textures_descriptor_allocator.Commit();
        UpdateTwoTexturesDescriptorSet(device, descriptor_set, sampler, src_depth_view,
                                       src_stencil_view);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptor_set,
                                  nullptr);
        BindBlitState(cmdbuf, layout, dst_region, src_region);
        cmdbuf.Draw(3, 1, 0, 0);
    });
    scheduler.InvalidateState();
}

void BlitImageHelper::BlitColorMSAA(const Framebuffer* dst_framebuffer,
                                    const ImageView& src_image_view, const Region2D& dst_region,
                                    const Region2D& src_region, Tegra::Engines::Fermi2D::Filter filter) {
    const BlitMSAAPipelineKey key{
        .renderpass = dst_framebuffer->RenderPass(),
        .samples = dst_framebuffer->Samples(),
        .format_class = FormatClass(src_image_view.format),
        .linear_filter = filter == Tegra::Engines::Fermi2D::Filter::Bilinear &&
                         !VideoCore::Surface::IsPixelFormatInteger(src_image_view.format),
        .source_samples = src_image_view.Samples(),
    };
    const VkPipelineLayout layout = *one_texture_pipeline_layout;
    const VkPipeline pipeline = FindOrEmplaceBlitColorMSAAPipeline(key);
    const VkSampler sampler = *nearest_sampler;
    const VkImageView src_view = src_image_view.Handle(Shader::TextureType::Color2D);
    RecordShaderReadBarrier(scheduler, src_image_view.ImageHandle(), VK_IMAGE_ASPECT_COLOR_BIT);
    scheduler.RequestRenderpass(dst_framebuffer);
    scheduler.Record([this, dst_region, src_region, pipeline, layout, sampler,
                      src_view](vk::CommandBuffer cmdbuf) {
        const VkDescriptorSet descriptor_set = one_texture_descriptor_allocator.Commit();
        UpdateOneTextureDescriptorSet(device, descriptor_set, sampler, src_view);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptor_set,
                                  nullptr);
        BindBlitState(cmdbuf, layout, dst_region, src_region);
        cmdbuf.Draw(3, 1, 0, 0);
    });
    scheduler.InvalidateState();
}

void BlitImageHelper::BlitDepthStencilMSAA(const Framebuffer* dst_framebuffer,
                                           ImageView& src_image_view, const Region2D& dst_region,
                                           const Region2D& src_region) {
    const bool has_stencil = dst_framebuffer->HasAspectStencilBit();
    const bool blit_stencil = has_stencil && device.IsExtShaderStencilExportSupported();
    if (has_stencil && !blit_stencil) {
        // Only the depth is copied. This can happen every frame, so do not flood the log.
        LOG_DEBUG(Render_Vulkan, "MSAA stencil blit needs VK_EXT_shader_stencil_export");
    }
    const BlitMSAAPipelineKey key{
        .renderpass = dst_framebuffer->RenderPass(),
        .samples = dst_framebuffer->Samples(),
        .format_class = MSAACopyFormatClass::Float,
        .source_samples = src_image_view.Samples(),
    };
    const VkPipelineLayout layout =
        blit_stencil ? *two_textures_pipeline_layout : *one_texture_pipeline_layout;
    const VkPipeline pipeline = FindOrEmplaceBlitDepthStencilMSAAPipeline(key, blit_stencil);
    const VkSampler sampler = *nearest_sampler;
    const VkImageView src_depth_view = src_image_view.DepthView();
    const VkImageView src_stencil_view =
        blit_stencil ? src_image_view.StencilView() : VkImageView{VK_NULL_HANDLE};
    const VkImageAspectFlags src_aspect =
        VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    RecordShaderReadBarrier(scheduler, src_image_view.ImageHandle(), src_aspect);
    scheduler.RequestRenderpass(dst_framebuffer);
    scheduler.Record([this, dst_region, src_region, pipeline, layout, sampler, src_depth_view,
                      src_stencil_view, blit_stencil](vk::CommandBuffer cmdbuf) {
        VkDescriptorSet descriptor_set{};
        if (blit_stencil) {
            descriptor_set = two_textures_descriptor_allocator.Commit();
            UpdateTwoTexturesDescriptorSet(device, descriptor_set, sampler, src_depth_view,
                                           src_stencil_view);
        } else {
            descriptor_set = one_texture_descriptor_allocator.Commit();
            UpdateOneTextureDescriptorSet(device, descriptor_set, sampler, src_depth_view);
        }
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptor_set,
                                  nullptr);
        BindBlitState(cmdbuf, layout, dst_region, src_region);
        cmdbuf.Draw(3, 1, 0, 0);
    });
    scheduler.InvalidateState();
}

// Resolve sample zero while retaining source samples and pixels outside the region.
bool BlitImageHelper::TryResolveDepthStencilMSAA(ImageView & dst, ImageView & src,
                                                 const Region2D & region)
{
    const VkFormat format = MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, src.format).format;
    VkImageAspectFlags aspect{};
    switch (format)
    {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        break;
    case VK_FORMAT_S8_UINT:
        aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
        break;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        break;
    default: return false;
    }
    const auto is_2d = [](const ImageView & view) {
        return view.type == VideoCommon::ImageViewType::e2D ||
               view.type == VideoCommon::ImageViewType::e2DArray ||
               view.type == VideoCommon::ImageViewType::Rect;
    };
    if (src.Samples() == VK_SAMPLE_COUNT_1_BIT ||
        dst.Samples() != VK_SAMPLE_COUNT_1_BIT || src.format != dst.format ||
        !device.SupportsDepthStencilResolve(aspect) || src.IsRescaled() || dst.IsRescaled() ||
        !is_2d(src) || !is_2d(dst) || src.range.extent.levels != 1 ||
        dst.range.extent.levels != 1 || src.range.extent.layers != 1 ||
        dst.range.extent.layers != 1 || region.start.x < 0 || region.start.y < 0 ||
        region.end.x <= region.start.x || region.end.y <= region.start.y)
    {
        return false;
    }
    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(static_cast<int>(src.Samples()));
    const u32 width = static_cast<u32>(region.end.x);
    const u32 height = static_cast<u32>(region.end.y);
    if (width > (src.size.width >> samples_x) || height > (src.size.height >> samples_y) ||
        width > dst.size.width || height > dst.size.height)
    {
        return false;
    }
    TickFrame();

    const u64 key = static_cast<u64>(format) | (static_cast<u64>(src.Samples()) << 32);
    auto found = std::ranges::find_if(native_resolve_passes, [key](const auto & entry) {
        return entry.first == key;
    });
    if (found == native_resolve_passes.end())
    {
        std::array<VkAttachmentDescription2, 2> attachments{};
        for (auto & attachment : attachments)
        {
            attachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
            attachment.format = format;
            attachment.loadOp = attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        attachments[0].samples = src.Samples();
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        VkAttachmentReference2 source{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        source.attachment = 0;
        source.layout = VK_IMAGE_LAYOUT_GENERAL;
        source.aspectMask = aspect;
        auto destination = source;
        destination.attachment = 1;
        VkSubpassDescriptionDepthStencilResolve resolve{
            VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
        resolve.depthResolveMode = (aspect & VK_IMAGE_ASPECT_DEPTH_BIT)
                                       ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                       : VK_RESOLVE_MODE_NONE;
        resolve.stencilResolveMode = (aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
                                         ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                         : VK_RESOLVE_MODE_NONE;
        resolve.pDepthStencilResolveAttachment = &destination;
        VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
        subpass.pNext = &resolve;
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &source;
        const VkRenderPassCreateInfo2 ci{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
            .attachmentCount = 2,
            .pAttachments = attachments.data(),
            .subpassCount = 1,
            .pSubpasses = &subpass};
        native_resolve_passes.emplace_back(key, device.GetLogical().CreateRenderPass2(ci));
        found = std::prev(native_resolve_passes.end());
    }
    const VkRenderPass pass = *found->second;
    const std::array views{src.RenderTarget(), dst.RenderTarget()};
    auto framebuffer = device.GetLogical().CreateFramebuffer({.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                              .renderPass = pass,
                                                              .attachmentCount = 2,
                                                              .pAttachments = views.data(),
                                                              .width = width,
                                                              .height = height,
                                                              .layers = 1});
    const VkRect2D area{
        {region.start.x, region.start.y},
        {static_cast<u32>(region.end.x - region.start.x),
         static_cast<u32>(region.end.y - region.start.y)}};
    const VkImageSubresourceRange source_range{
        aspect, static_cast<u32>(src.range.base.level), 1,
        static_cast<u32>(src.range.base.layer), 1};
    const VkImageSubresourceRange destination_range{
        aspect, static_cast<u32>(dst.range.base.level), 1,
        static_cast<u32>(dst.range.base.layer), 1};
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([pass, handle = *framebuffer, area, source_range, destination_range,
                      source = src.ImageHandle(), destination = dst.ImageHandle()](vk::CommandBuffer cmdbuf) {
        const auto barrier = [](VkImage image, VkImageSubresourceRange range,
                                VkAccessFlags from, VkAccessFlags to) {
            return VkImageMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = from,
                .dstAccessMask = to,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = range};
        };
        const std::array before{
            barrier(source, source_range, VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT),
            barrier(destination, destination_range, VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)};
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               0, nullptr, nullptr, before);
        const VkRenderPassBeginInfo bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = pass,
            .framebuffer = handle,
            .renderArea = area};
        cmdbuf.BeginRenderPass(bi, VK_SUBPASS_CONTENTS_INLINE);
        cmdbuf.EndRenderPass();
        const std::array after{
            barrier(source, source_range, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
            barrier(destination, destination_range, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)};
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, nullptr, after);
    });
    msaa_copy_resources.push_back(MSAACopyResources{
        .tick = scheduler.CurrentTick(), .framebuffer = std::move(framebuffer)});
    scheduler.InvalidateState();
    return true;
}

void BlitImageHelper::ConvertD32ToR32(const Framebuffer* dst_framebuffer,
                                      const ImageView& src_image_view) {
    ConvertDepthToColorPipeline(convert_d32_to_r32_pipeline, dst_framebuffer->RenderPass());
    Convert(*convert_d32_to_r32_pipeline, dst_framebuffer, src_image_view);
}

void BlitImageHelper::ConvertR32ToD32(const Framebuffer* dst_framebuffer,
                                      const ImageView& src_image_view) {
    ConvertColorToDepthPipeline(convert_r32_to_d32_pipeline, dst_framebuffer->RenderPass());
    Convert(*convert_r32_to_d32_pipeline, dst_framebuffer, src_image_view);
}

void BlitImageHelper::ConvertD16ToR16(const Framebuffer* dst_framebuffer,
                                      const ImageView& src_image_view) {
    ConvertDepthToColorPipeline(convert_d16_to_r16_pipeline, dst_framebuffer->RenderPass());
    Convert(*convert_d16_to_r16_pipeline, dst_framebuffer, src_image_view);
}

void BlitImageHelper::ConvertR16ToD16(const Framebuffer* dst_framebuffer,
                                      const ImageView& src_image_view) {
    ConvertColorToDepthPipeline(convert_r16_to_d16_pipeline, dst_framebuffer->RenderPass());
    Convert(*convert_r16_to_d16_pipeline, dst_framebuffer, src_image_view);
}

void BlitImageHelper::ConvertABGR8ToD24S8(const Framebuffer* dst_framebuffer,
                                          const ImageView& src_image_view) {
    ConvertPipelineDepthStencilTargetEx(convert_abgr8_to_d24s8_pipeline,
                                        dst_framebuffer->RenderPass(),
                                        convert_abgr8_to_d24s8_frag);
    Convert(*convert_abgr8_to_d24s8_pipeline, dst_framebuffer, src_image_view);
}

void BlitImageHelper::ConvertABGR8ToS8D24(const Framebuffer* dst_framebuffer,
                                          const ImageView& src_image_view) {
    ConvertPipelineDepthStencilTargetEx(convert_abgr8_to_s8d24_pipeline,
                                        dst_framebuffer->RenderPass(),
                                        convert_abgr8_to_s8d24_frag);
    Convert(*convert_abgr8_to_s8d24_pipeline, dst_framebuffer, src_image_view);
}

void BlitImageHelper::ConvertABGR8ToD32F(const Framebuffer* dst_framebuffer,
                                         const ImageView& src_image_view) {
    ConvertPipelineDepthTargetEx(convert_abgr8_to_d32f_pipeline, dst_framebuffer->RenderPass(),
                                 convert_abgr8_to_d32f_frag);
    Convert(*convert_abgr8_to_d32f_pipeline, dst_framebuffer, src_image_view);
}

void BlitImageHelper::ConvertD32FToABGR8(const Framebuffer* dst_framebuffer,
                                         ImageView& src_image_view) {
    ConvertPipelineColorTargetEx(convert_d32f_to_abgr8_pipeline, dst_framebuffer->RenderPass(),
                                 convert_d32f_to_abgr8_frag);
    ConvertDepthStencil(*convert_d32f_to_abgr8_pipeline, dst_framebuffer, src_image_view);
}

void BlitImageHelper::ConvertD24S8ToABGR8(const Framebuffer* dst_framebuffer,
                                          ImageView& src_image_view) {
    ConvertPipelineColorTargetEx(convert_d24s8_to_abgr8_pipeline, dst_framebuffer->RenderPass(),
                                 convert_d24s8_to_abgr8_frag);
    ConvertDepthStencil(*convert_d24s8_to_abgr8_pipeline, dst_framebuffer, src_image_view);
}

void BlitImageHelper::ConvertS8D24ToABGR8(const Framebuffer* dst_framebuffer,
                                          ImageView& src_image_view) {
    ConvertPipelineColorTargetEx(convert_s8d24_to_abgr8_pipeline, dst_framebuffer->RenderPass(),
                                 convert_s8d24_to_abgr8_frag);
    ConvertDepthStencil(*convert_s8d24_to_abgr8_pipeline, dst_framebuffer, src_image_view);
}

void BlitImageHelper::ClearColor(const Framebuffer* dst_framebuffer, u8 color_mask,
                                 const std::array<f32, 4>& clear_color,
                                 const Region2D& dst_region) {
    const BlitImagePipelineKey key{
        .renderpass = dst_framebuffer->RenderPass(),
        .operation = Tegra::Engines::Fermi2D::Operation::BlendPremult,
    };
    const VkPipeline pipeline = FindOrEmplaceClearColorPipeline(key);
    const VkPipelineLayout layout = *clear_color_pipeline_layout;
    scheduler.RequestRenderpass(dst_framebuffer);
    scheduler.Record(
        [pipeline, layout, color_mask, clear_color, dst_region](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            const std::array blend_color = {
                (color_mask & 0x1) ? 1.0f : 0.0f, (color_mask & 0x2) ? 1.0f : 0.0f,
                (color_mask & 0x4) ? 1.0f : 0.0f, (color_mask & 0x8) ? 1.0f : 0.0f};
            cmdbuf.SetBlendConstants(blend_color.data());
            BindBlitState(cmdbuf, dst_region);
            cmdbuf.PushConstants(layout, VK_SHADER_STAGE_FRAGMENT_BIT, clear_color);
            cmdbuf.Draw(3, 1, 0, 0);
        });
    scheduler.InvalidateState();
}

void BlitImageHelper::ClearColor(const Framebuffer* dst_framebuffer, u8 color_mask,
                                 const VkClearColorValue& clear_color,
                                 VideoCore::Surface::PixelFormat format,
                                 const Region2D& dst_region) {
    const bool integer_format = VideoCore::Surface::IsPixelFormatInteger(format);
    const u8 color_type =
        integer_format ? (VideoCore::Surface::IsPixelFormatSignedInteger(format) ? 1 : 2) : 0;
    const BlitImagePipelineKey key{
        .renderpass = dst_framebuffer->RenderPass(),
        .operation = Tegra::Engines::Fermi2D::Operation::BlendPremult,
        .samples = dst_framebuffer->Samples(),
        .clear_color_mask = color_mask,
        .clear_color_type = color_type,
        .masked_clear = true,
    };
    const VkPipeline pipeline = FindOrEmplaceClearColorPipeline(key);
    const VkPipelineLayout layout = *clear_color_pipeline_layout;
    scheduler.RequestRenderpass(dst_framebuffer);
    scheduler.Record([pipeline, layout, clear_color, dst_region](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        BindBlitState(cmdbuf, dst_region);
        cmdbuf.PushConstants(layout, VK_SHADER_STAGE_FRAGMENT_BIT, clear_color);
        cmdbuf.Draw(3, 1, 0, 0);
    });
    scheduler.InvalidateState();
}

void BlitImageHelper::ClearDepthStencil(const Framebuffer* dst_framebuffer, bool depth_clear,
                                        f32 clear_depth, u8 stencil_mask, u32 stencil_ref,
                                        u32 stencil_compare_mask, const Region2D& dst_region) {
    const BlitDepthStencilPipelineKey key{
        .renderpass = dst_framebuffer->RenderPass(),
        .depth_clear = depth_clear,
        .stencil_mask = stencil_mask,
        .stencil_compare_mask = stencil_compare_mask,
        .stencil_ref = stencil_ref,
        .samples = device.GetDriverID() == VK_DRIVER_ID_MOLTENVK ? dst_framebuffer->Samples()
                                                                 : VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipeline pipeline = FindOrEmplaceClearStencilPipeline(key);
    const VkPipelineLayout layout = *clear_color_pipeline_layout;
    scheduler.RequestRenderpass(dst_framebuffer);
    scheduler.Record([pipeline, layout, clear_depth, dst_region](vk::CommandBuffer cmdbuf) {
        constexpr std::array blend_constants{0.0f, 0.0f, 0.0f, 0.0f};
        cmdbuf.SetBlendConstants(blend_constants.data());
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        BindBlitState(cmdbuf, dst_region);
        cmdbuf.PushConstants(layout, VK_SHADER_STAGE_FRAGMENT_BIT, clear_depth);
        cmdbuf.Draw(3, 1, 0, 0);
    });
    scheduler.InvalidateState();
}

void BlitImageHelper::TickFrame() {
    while (!msaa_copy_resources.empty() && scheduler.IsFree(msaa_copy_resources.front().tick)) {
        msaa_copy_resources.pop_front();
    }
}

void BlitImageHelper::CopyToMSAAImpl(VkRenderPass renderpass, VkPipeline pipeline,
                                      VkPipelineLayout layout, VkImage dst_image,
                                      VkFormat dst_format, VkImage src_image, VkFormat src_format,
                                      s32 scale_x, s32 scale_y,
                                      std::span<const VideoCommon::ImageCopy> copies,
                                      const MSAACopyAspectInfo& aspect_info, bool copy_stencil,
                                      s32 packed_format) {
    TickFrame();
    if (copies.empty()) return;
    const bool batch_regions = src_image != dst_image;
    struct Job
    {
        u32 src_level, src_layer, dst_level, dst_layer;
        VkRect2D area;
        MSAACopyPushConstants push;
        bool SameSubresources(const Job & other) const
        {
            return src_level == other.src_level && src_layer == other.src_layer &&
                   dst_level == other.dst_level && dst_layer == other.dst_layer;
        }
    };
    boost::container::small_vector<Job, 16> jobs;
    for (const auto & copy : copies)
    {
        const s32 layers = (std::min)(copy.src_subresource.num_layers, copy.dst_subresource.num_layers);
        for (s32 layer = 0; layer < layers; ++layer)
        {
            jobs.push_back(Job{
                static_cast<u32>(copy.src_subresource.base_level),
                static_cast<u32>(copy.src_subresource.base_layer + layer),
                static_cast<u32>(copy.dst_subresource.base_level),
                static_cast<u32>(copy.dst_subresource.base_layer + layer),
                {{copy.dst_offset.x, copy.dst_offset.y}, {copy.extent.width, copy.extent.height}},
                {{copy.dst_offset.x, copy.dst_offset.y}, {copy.src_offset.x, copy.src_offset.y}, {scale_x, scale_y}, packed_format}});
        }
    }
    // Bound the linear local cache to avoid quadratic planning on unusually large uploads.
    const bool reuse_resources = jobs.size() <= 256;
    struct Cached
    {
        Job subresource;
        VkExtent2D extent;
        VkImageView source, stencil;
        VkFramebuffer framebuffer;
    };
    // Only reuse within this upload: both images remain alive and their handles cannot be recycled.
    boost::container::small_vector<Cached, 16> cache;
    const VkSampler sampler = *nearest_sampler;
    for (size_t begin = 0; begin < jobs.size();)
    {
        const auto & job = jobs[begin];
        size_t end = begin + 1;
        if (batch_regions)
        {
            while (end < jobs.size() && job.SameSubresources(jobs[end]))
                ++end;
        }
        const auto found = reuse_resources ? std::ranges::find_if(cache, [&](const Cached & c) {
            return job.SameSubresources(c.subresource);
        })
                                           : cache.end();
        VkExtent2D extent{static_cast<u32>(job.area.offset.x) + job.area.extent.width,
                          static_cast<u32>(job.area.offset.y) + job.area.extent.height};
        if (found != cache.end())
        {
            extent = found->extent;
        }
        else if (reuse_resources || batch_regions)
        {
            const size_t first = reuse_resources ? 0 : begin;
            const size_t last = reuse_resources ? jobs.size() : end;
            for (size_t index = first; index < last; ++index)
            {
                const auto & next = jobs[index];
                if (!job.SameSubresources(next)) continue;
                extent.width = (std::max)(extent.width, static_cast<u32>(next.area.offset.x) + next.area.extent.width);
                extent.height = (std::max)(extent.height, static_cast<u32>(next.area.offset.y) + next.area.extent.height);
            }
        }
        Cached handles{};
        if (found != cache.end())
        {
            handles = *found;
        }
        else
        {
            auto source = MakeMSAACopyView(device.GetLogical(), src_image, src_format,
                                           job.src_level, job.src_layer, aspect_info.src_view_aspect);
            auto destination = MakeMSAACopyView(device.GetLogical(), dst_image, dst_format,
                                                job.dst_level, job.dst_layer, aspect_info.attachment_aspect);
            vk::ImageView stencil;
            if (copy_stencil) stencil = MakeMSAACopyView(device.GetLogical(), src_image, src_format,
                                                         job.src_level, job.src_layer, VK_IMAGE_ASPECT_STENCIL_BIT);
            auto framebuffer = device.GetLogical().CreateFramebuffer({.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                                                      .renderPass = renderpass,
                                                                      .attachmentCount = 1,
                                                                      .pAttachments = destination.address(),
                                                                      .width = extent.width,
                                                                      .height = extent.height,
                                                                      .layers = 1});
            handles = Cached{job, extent, *source, copy_stencil ? *stencil : VK_NULL_HANDLE, *framebuffer};
            msaa_copy_resources.push_back(MSAACopyResources{
                .tick = scheduler.CurrentTick(), .src_view = std::move(source), .dst_view = std::move(destination), .framebuffer = std::move(framebuffer)});
            if (copy_stencil) msaa_copy_resources.push_back(MSAACopyResources{
                .tick = scheduler.CurrentTick(), .src_view = std::move(stencil)});
            if (reuse_resources) cache.push_back(handles);
        }
        boost::container::small_vector<Job, 1> draws(jobs.begin() + begin, jobs.begin() + end);
        const VkRect2D render_area = batch_regions ? VkRect2D{{0, 0}, extent} : job.area;
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([this, pipeline, layout, sampler, renderpass,
                          framebuffer_handle = handles.framebuffer, src_view_handle = handles.source,
                          src_stencil_handle = handles.stencil, src = src_image, dst = dst_image,
                          src_level = job.src_level, src_layer = job.src_layer, dst_level = job.dst_level,
                          dst_layer = job.dst_layer, aspect_info, render_area,
                          draws = std::move(draws)](vk::CommandBuffer cmdbuf) {
            const VkImageSubresourceRange src_barrier_range{
                .aspectMask = aspect_info.src_barrier_aspect,
                .baseMipLevel = src_level,
                .levelCount = 1,
                .baseArrayLayer = src_layer,
                .layerCount = 1,
            };
            const VkImageSubresourceRange dst_barrier_range{
                .aspectMask = aspect_info.dst_barrier_aspect,
                .baseMipLevel = dst_level,
                .levelCount = 1,
                .baseArrayLayer = dst_layer,
                .layerCount = 1,
            };
            const std::array pre_barriers{
                VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = aspect_info.pre_src_access,
                    .dstAccessMask = aspect_info.pre_src_dst_access,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = src,
                    .subresourceRange = src_barrier_range,
                },
                VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = aspect_info.pre_dst_access,
                    .dstAccessMask = aspect_info.pre_dst_dst_access,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = dst,
                    .subresourceRange = dst_barrier_range,
                },
            };
            cmdbuf.PipelineBarrier(aspect_info.pre_src_stages, aspect_info.pre_dst_stages, 0,
                                   nullptr, nullptr, pre_barriers);

            const VkRenderPassBeginInfo bi{
                .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = renderpass, .framebuffer = framebuffer_handle, .renderArea = render_area};
            cmdbuf.BeginRenderPass(bi, VK_SUBPASS_CONTENTS_INLINE);
            VkDescriptorSet descriptor_set;
            if (src_stencil_handle != VK_NULL_HANDLE)
            {
                descriptor_set = two_textures_descriptor_allocator.Commit();
                UpdateTwoTexturesDescriptorSet(device, descriptor_set, sampler, src_view_handle, src_stencil_handle);
            }
            else
            {
                descriptor_set = one_texture_descriptor_allocator.Commit();
                UpdateOneTextureDescriptorSet(device, descriptor_set, sampler, src_view_handle);
            }
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptor_set, nullptr);
            for (const auto & draw : draws)
            {
                const VkViewport viewport{
                    .x = static_cast<float>(draw.area.offset.x), .y = static_cast<float>(draw.area.offset.y), .width = static_cast<float>(draw.area.extent.width), .height = static_cast<float>(draw.area.extent.height), .minDepth = 0.0f, .maxDepth = 1.0f};
                cmdbuf.SetViewport(0, viewport);
                cmdbuf.SetScissor(0, draw.area);
                cmdbuf.PushConstants(layout, VK_SHADER_STAGE_FRAGMENT_BIT, draw.push);
                cmdbuf.Draw(3, 1, 0, 0);
            }
            cmdbuf.EndRenderPass();
            const VkImageMemoryBarrier post_barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = aspect_info.post_src_access,
                .dstAccessMask = aspect_info.post_dst_access,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst,
                .subresourceRange = dst_barrier_range,
            };
            cmdbuf.PipelineBarrier(aspect_info.post_src_stages, aspect_info.post_dst_stages, 0,
                                   post_barrier);
        });
        begin = end;
    }
    scheduler.InvalidateState();
}

void BlitImageHelper::CopyToMSAA(RenderPassCache& render_pass_cache, VkImage dst_image,
                                 VideoCore::Surface::PixelFormat format, VkImage src_image,
                                 u32 num_samples,
                                 std::span<const VideoCommon::ImageCopy> copies) {
    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(static_cast<int>(num_samples));
    const s32 scale_x = 1 << samples_x;
    const s32 scale_y = 1 << samples_y;
    const VkSampleCountFlagBits samples = SampleCountFlag(num_samples);
    RenderPassKey renderpass_key{};
    renderpass_key.color_formats.fill(VideoCore::Surface::PixelFormat::Invalid);
    renderpass_key.color_formats[0] = format;
    renderpass_key.depth_format = VideoCore::Surface::PixelFormat::Invalid;
    renderpass_key.samples = samples;
    const VkRenderPass renderpass = render_pass_cache.Get(renderpass_key);
    const MSAACopyPipelineKey key{
        .renderpass = renderpass,
        .samples = samples,
        .format_class = FormatClass(format),
    };
    const MSAACopyAspectInfo aspect_info{
        .src_view_aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .attachment_aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .src_barrier_aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .dst_barrier_aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .pre_src_access = VK_ACCESS_TRANSFER_WRITE_BIT,
        .pre_dst_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                          VK_ACCESS_TRANSFER_WRITE_BIT,
        .pre_src_dst_access = VK_ACCESS_SHADER_READ_BIT,
        .pre_dst_dst_access =
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .pre_src_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        .pre_dst_stages =
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .post_src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .post_dst_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        .post_src_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .post_dst_stages = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT,
    };
    const VkFormat vk_format =
        MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, format).format;
    CopyToMSAAImpl(renderpass, FindOrEmplaceMSAACopyPipeline(key), *msaa_copy_pipeline_layout,
                   dst_image, vk_format, src_image, vk_format, scale_x, scale_y, copies,
                   aspect_info, false);
}

void BlitImageHelper::CopyToMSAADepth(RenderPassCache& render_pass_cache, VkImage dst_image,
                                      VideoCore::Surface::PixelFormat format, VkImage src_image,
                                      u32 num_samples,
                                      std::span<const VideoCommon::ImageCopy> copies,
                                      bool copy_stencil) {
    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(static_cast<int>(num_samples));
    const s32 scale_x = 1 << samples_x;
    const s32 scale_y = 1 << samples_y;
    const VkSampleCountFlagBits samples = SampleCountFlag(num_samples);
    RenderPassKey renderpass_key{};
    renderpass_key.color_formats.fill(VideoCore::Surface::PixelFormat::Invalid);
    renderpass_key.depth_format = format;
    renderpass_key.samples = samples;
    const VkRenderPass renderpass = render_pass_cache.Get(renderpass_key);
    const MSAACopyPipelineKey key{
        .renderpass = renderpass,
        .samples = samples,
        .format_class = MSAACopyFormatClass::Float,
    };
    VkImageAspectFlags attachment_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (VideoCore::Surface::GetFormatType(format) ==
        VideoCore::Surface::SurfaceType::DepthStencil) {
        attachment_aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    const VkPipelineLayout layout = copy_stencil ? *msaa_copy_depth_stencil_pipeline_layout
                                                 : *msaa_copy_pipeline_layout;
    const MSAACopyAspectInfo aspect_info{
        .src_view_aspect = VK_IMAGE_ASPECT_DEPTH_BIT,
        .attachment_aspect = attachment_aspect,
        .src_barrier_aspect = attachment_aspect,
        .dst_barrier_aspect = attachment_aspect,
        .pre_src_access = VK_ACCESS_TRANSFER_WRITE_BIT,
        .pre_dst_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .pre_src_dst_access = VK_ACCESS_SHADER_READ_BIT,
        .pre_dst_dst_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .pre_src_stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        .pre_dst_stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .post_src_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .post_dst_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        .post_src_stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .post_dst_stages = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
    };
    const VkFormat vk_format =
        MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, format).format;
    CopyToMSAAImpl(renderpass, FindOrEmplaceMSAACopyDepthPipeline(key, copy_stencil), layout,
                   dst_image, vk_format, src_image, vk_format, scale_x, scale_y, copies,
                   aspect_info, copy_stencil);
}

void BlitImageHelper::CopyPackedDepthStencilToMSAA(
    RenderPassCache& render_pass_cache, VkImage dst_image,
    VideoCore::Surface::PixelFormat format, VkImage src_image, VkFormat src_format,
    u32 num_samples, std::span<const VideoCommon::ImageCopy> copies, s32 packed_format,
    bool copy_stencil) {
    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(static_cast<int>(num_samples));
    const s32 scale_x = 1 << samples_x;
    const s32 scale_y = 1 << samples_y;
    const VkSampleCountFlagBits samples = SampleCountFlag(num_samples);
    RenderPassKey renderpass_key{};
    renderpass_key.color_formats.fill(VideoCore::Surface::PixelFormat::Invalid);
    renderpass_key.depth_format = format;
    renderpass_key.samples = samples;
    const VkRenderPass renderpass = render_pass_cache.Get(renderpass_key);
    const MSAACopyPipelineKey key{
        .renderpass = renderpass,
        .samples = samples,
        .format_class = MSAACopyFormatClass::Float,
        .packed_depth_stencil = true,
    };
    const MSAACopyAspectInfo aspect_info{
        .src_view_aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .attachment_aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
        .src_barrier_aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .dst_barrier_aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
        .pre_src_access = VK_ACCESS_TRANSFER_WRITE_BIT,
        .pre_dst_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .pre_src_dst_access = VK_ACCESS_SHADER_READ_BIT,
        .pre_dst_dst_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .pre_src_stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                          VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        .pre_dst_stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .post_src_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .post_dst_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
        .post_src_stages = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .post_dst_stages = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
    };
    const VkFormat dst_format =
        MaxwellToVK::SurfaceFormat(device, FormatType::Optimal, true, format).format;
    CopyToMSAAImpl(renderpass, FindOrEmplaceMSAACopyDepthPipeline(key, copy_stencil),
                   *msaa_copy_pipeline_layout, dst_image, dst_format, src_image, src_format,
                   scale_x, scale_y, copies, aspect_info, false, packed_format);
}

void BlitImageHelper::Convert(VkPipeline pipeline, const Framebuffer* dst_framebuffer,
                              const ImageView& src_image_view) {
    const VkPipelineLayout layout = *one_texture_pipeline_layout;
    const VkImageView src_view = src_image_view.Handle(Shader::TextureType::Color2D);
    const VkSampler sampler = *nearest_sampler;
    const VkExtent2D extent = GetConversionExtent(src_image_view);

    scheduler.RequestRenderpass(dst_framebuffer);
    scheduler.Record([pipeline, layout, sampler, src_view, extent, this](vk::CommandBuffer cmdbuf) {
        const VkOffset2D offset{
            .x = 0,
            .y = 0,
        };
        const VkViewport viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(extent.width),
            .height = static_cast<float>(extent.height),
            .minDepth = 0.0f,
            .maxDepth = 0.0f,
        };
        const VkRect2D scissor{
            .offset = offset,
            .extent = extent,
        };
        const PushConstants push_constants{
            .tex_scale = {viewport.width, viewport.height},
            .tex_offset = {0.0f, 0.0f},
        };
        const VkDescriptorSet descriptor_set = one_texture_descriptor_allocator.Commit();
        UpdateOneTextureDescriptorSet(device, descriptor_set, sampler, src_view);

        // TODO: Barriers
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptor_set,
                                  nullptr);
        cmdbuf.SetViewport(0, viewport);
        cmdbuf.SetScissor(0, scissor);
        cmdbuf.PushConstants(layout, VK_SHADER_STAGE_VERTEX_BIT, push_constants);
        cmdbuf.Draw(3, 1, 0, 0);
    });
    scheduler.InvalidateState();
}

void BlitImageHelper::ConvertDepthStencil(VkPipeline pipeline, const Framebuffer* dst_framebuffer,
                                          ImageView& src_image_view) {
    const VkPipelineLayout layout = *two_textures_pipeline_layout;
    const VkImageView src_depth_view = src_image_view.DepthView();
    const VkImageView src_stencil_view = src_image_view.StencilView();
    const VkSampler sampler = *nearest_sampler;
    const VkExtent2D extent = GetConversionExtent(src_image_view);

    scheduler.RequestRenderpass(dst_framebuffer);
    scheduler.Record([pipeline, layout, sampler, src_depth_view, src_stencil_view, extent,
                      this](vk::CommandBuffer cmdbuf) {
        const VkOffset2D offset{
            .x = 0,
            .y = 0,
        };
        const VkViewport viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(extent.width),
            .height = static_cast<float>(extent.height),
            .minDepth = 0.0f,
            .maxDepth = 0.0f,
        };
        const VkRect2D scissor{
            .offset = offset,
            .extent = extent,
        };
        const PushConstants push_constants{
            .tex_scale = {viewport.width, viewport.height},
            .tex_offset = {0.0f, 0.0f},
        };
        const VkDescriptorSet descriptor_set = two_textures_descriptor_allocator.Commit();
        UpdateTwoTexturesDescriptorSet(device, descriptor_set, sampler, src_depth_view,
                                       src_stencil_view);
        // TODO: Barriers
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, descriptor_set,
                                  nullptr);
        cmdbuf.SetViewport(0, viewport);
        cmdbuf.SetScissor(0, scissor);
        cmdbuf.PushConstants(layout, VK_SHADER_STAGE_VERTEX_BIT, push_constants);
        cmdbuf.Draw(3, 1, 0, 0);
    });
    scheduler.InvalidateState();
}

VkPipeline BlitImageHelper::FindOrEmplaceColorPipeline(const BlitImagePipelineKey& key) {
    const auto it = std::ranges::find(blit_color_keys, key);
    if (it != blit_color_keys.end()) {
        return *blit_color_pipelines[std::distance(blit_color_keys.begin(), it)];
    }
    blit_color_keys.push_back(key);

    const std::array stages = MakeStages(*full_screen_vert, *blit_color_to_color_frag);
    const VkPipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    std::array<VkPipelineColorBlendAttachmentState, NUM_RT> blend_attachments{};
    blend_attachments[0] = blend_attachment;
    auto multisample = PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = key.samples;
    auto depth_stencil = PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = VK_FALSE;
    depth_stencil.depthWriteEnable = VK_FALSE;
    // Only location 0 is written. Keep the remaining subpass slots explicitly disabled.
    // TODO: programmable blending
    const VkPipelineColorBlendStateCreateInfo color_blend_create_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_CLEAR,
        .attachmentCount = key.color_attachment_count,
        .pAttachments = blend_attachments.data(),
        .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    blit_color_pipelines.push_back(device.GetLogical().CreateGraphicsPipeline({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pInputAssemblyState = &PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pTessellationState = nullptr,
        .pViewportState = &PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pRasterizationState = &PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pMultisampleState = &multisample,
        .pDepthStencilState =
            device.GetDriverID() == VK_DRIVER_ID_MOLTENVK ? &depth_stencil : nullptr,
        .pColorBlendState = &color_blend_create_info,
        .pDynamicState = &PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .layout = *one_texture_pipeline_layout,
        .renderPass = key.renderpass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    }));
    return *blit_color_pipelines.back();
}

VkPipeline BlitImageHelper::FindOrEmplaceDepthStencilPipeline(const BlitImagePipelineKey& key) {
    const auto it = std::ranges::find(blit_depth_stencil_keys, key);
    if (it != blit_depth_stencil_keys.end()) {
        return *blit_depth_stencil_pipelines[std::distance(blit_depth_stencil_keys.begin(), it)];
    }
    blit_depth_stencil_keys.push_back(key);
    const std::array stages = MakeStages(*full_screen_vert, *blit_depth_stencil_frag);
    blit_depth_stencil_pipelines.push_back(device.GetLogical().CreateGraphicsPipeline({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pInputAssemblyState = &PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pTessellationState = nullptr,
        .pViewportState = &PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pRasterizationState = &PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pMultisampleState = &PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pDepthStencilState = &PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pColorBlendState = &PIPELINE_COLOR_BLEND_STATE_GENERIC_CREATE_INFO,
        .pDynamicState = &PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .layout = *two_textures_pipeline_layout,
        .renderPass = key.renderpass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    }));
    return *blit_depth_stencil_pipelines.back();
}

VkPipeline BlitImageHelper::FindOrEmplaceClearColorPipeline(const BlitImagePipelineKey& key) {
    const auto it = std::ranges::find(clear_color_keys, key);
    if (it != clear_color_keys.end()) {
        return *clear_color_pipelines[std::distance(clear_color_keys.begin(), it)];
    }
    clear_color_keys.push_back(key);
    vk::ShaderModule* fragment = &clear_color_frag;
    if (key.clear_color_type == 1) {
        if (!clear_color_sint_frag) {
            clear_color_sint_frag = BuildShader(device, VULKAN_COLOR_CLEAR_SINT_FRAG_SPV);
        }
        fragment = &clear_color_sint_frag;
    } else if (key.clear_color_type == 2) {
        if (!clear_color_uint_frag) {
            clear_color_uint_frag = BuildShader(device, VULKAN_COLOR_CLEAR_UINT_FRAG_SPV);
        }
        fragment = &clear_color_uint_frag;
    }
    const std::array stages = MakeStages(*clear_color_vert, **fragment);
    auto multisample = PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = key.samples;
    const VkPipelineColorBlendAttachmentState color_blend_attachment_state{
        .blendEnable = !key.masked_clear,
        .srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = key.clear_color_mask,
    };
    const VkPipelineColorBlendStateCreateInfo color_blend_state_generic_create_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_CLEAR,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment_state,
        .blendConstants = {0.0f, 0.0f, 0.0f, 0.0f},
    };
    clear_color_pipelines.push_back(device.GetLogical().CreateGraphicsPipeline({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pInputAssemblyState = &PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pTessellationState = nullptr,
        .pViewportState = &PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pRasterizationState = &PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pColorBlendState = &color_blend_state_generic_create_info,
        .pDynamicState = &PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .layout = *clear_color_pipeline_layout,
        .renderPass = key.renderpass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    }));
    return *clear_color_pipelines.back();
}

VkPipeline BlitImageHelper::FindOrEmplaceClearStencilPipeline(
    const BlitDepthStencilPipelineKey& key) {
    const auto it = std::ranges::find(clear_stencil_keys, key);
    if (it != clear_stencil_keys.end()) {
        return *clear_stencil_pipelines[std::distance(clear_stencil_keys.begin(), it)];
    }
    clear_stencil_keys.push_back(key);
    const std::array stages = MakeStages(*clear_color_vert, *clear_stencil_frag);
    auto multisample = PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = key.samples;
    const auto stencil = VkStencilOpState{
        .failOp = VK_STENCIL_OP_KEEP,
        .passOp = VK_STENCIL_OP_REPLACE,
        .depthFailOp = VK_STENCIL_OP_KEEP,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .compareMask = key.stencil_compare_mask,
        .writeMask = key.stencil_mask,
        .reference = key.stencil_ref,
    };
    const VkPipelineDepthStencilStateCreateInfo depth_stencil_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = key.depth_clear,
        .depthWriteEnable = key.depth_clear,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_TRUE,
        .front = stencil,
        .back = stencil,
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 0.0f,
    };
    clear_stencil_pipelines.push_back(device.GetLogical().CreateGraphicsPipeline({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pInputAssemblyState = &PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pTessellationState = nullptr,
        .pViewportState = &PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pRasterizationState = &PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pMultisampleState = &multisample,
        .pDepthStencilState = &depth_stencil_ci,
        .pColorBlendState = &PIPELINE_COLOR_BLEND_STATE_GENERIC_CREATE_INFO,
        .pDynamicState = &PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .layout = *clear_color_pipeline_layout,
        .renderPass = key.renderpass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    }));
    return *clear_stencil_pipelines.back();
}

VkPipeline BlitImageHelper::FindOrEmplaceMSAACopyPipeline(const MSAACopyPipelineKey& key) {
    const size_t index = FindCachedKey(msaa_copy_keys, key, msaa_copy_last);
    if (index != msaa_copy_keys.size())
    {
        return *msaa_copy_pipelines[index];
    }
    msaa_copy_keys.push_back(key);
    VkShaderModule frag_module = *convert_non_msaa_to_msaa_frag;
    if (key.format_class == MSAACopyFormatClass::SignedInteger) {
        frag_module = *convert_non_msaa_to_msaa_sint_frag;
    } else if (key.format_class == MSAACopyFormatClass::UnsignedInteger) {
        frag_module = *convert_non_msaa_to_msaa_uint_frag;
    }
    const std::array stages = MakeStages(*clear_color_vert, frag_module);
    const VkPipelineMultisampleStateCreateInfo multisample_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = key.samples,
        .sampleShadingEnable = VK_TRUE,
        .minSampleShading = 1.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    msaa_copy_pipelines.push_back(device.GetLogical().CreateGraphicsPipeline({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pInputAssemblyState = &PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pTessellationState = nullptr,
        .pViewportState = &PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pRasterizationState = &PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pMultisampleState = &multisample_ci,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &PIPELINE_COLOR_BLEND_STATE_GENERIC_CREATE_INFO,
        .pDynamicState = &PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .layout = *msaa_copy_pipeline_layout,
        .renderPass = key.renderpass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    }));
    return *msaa_copy_pipelines.back();
}

VkPipeline BlitImageHelper::FindOrEmplaceMSAACopyDepthPipeline(const MSAACopyPipelineKey& key,
                                                               bool copy_stencil) {
    auto& keys = copy_stencil ? msaa_copy_depth_stencil_keys : msaa_copy_depth_keys;
    auto& pipelines = copy_stencil ? msaa_copy_depth_stencil_pipelines : msaa_copy_depth_pipelines;
    auto & last = copy_stencil ? msaa_copy_depth_stencil_last : msaa_copy_depth_last;
    const size_t index = FindCachedKey(keys, key, last);
    if (index != keys.size())
    {
        return *pipelines[index];
    }
    keys.push_back(key);
    const bool writes_stencil = copy_stencil;
    const VkShaderModule frag_module =
        key.packed_depth_stencil
            ? (copy_stencil ? *convert_packed_depth_stencil_to_msaa_frag
                            : *convert_packed_depth_to_msaa_frag)
            : (copy_stencil ? *convert_non_msaa_to_msaa_depth_stencil_frag
                            : *convert_non_msaa_to_msaa_depth_frag);
    const std::array stages = MakeStages(*clear_color_vert, frag_module);
    const VkPipelineMultisampleStateCreateInfo multisample_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = key.samples,
        .sampleShadingEnable = VK_TRUE,
        .minSampleShading = 1.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    static constexpr VkStencilOpState REPLACE_STENCIL_OP{
        .failOp = VK_STENCIL_OP_REPLACE,
        .passOp = VK_STENCIL_OP_REPLACE,
        .depthFailOp = VK_STENCIL_OP_REPLACE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .compareMask = 0xFF,
        .writeMask = 0xFF,
        .reference = 0,
    };
    const VkPipelineDepthStencilStateCreateInfo depth_stencil_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = writes_stencil ? VK_TRUE : VK_FALSE,
        .front = writes_stencil ? REPLACE_STENCIL_OP : VkStencilOpState{},
        .back = writes_stencil ? REPLACE_STENCIL_OP : VkStencilOpState{},
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 0.0f,
    };
    pipelines.push_back(device.GetLogical().CreateGraphicsPipeline({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pInputAssemblyState = &PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pTessellationState = nullptr,
        .pViewportState = &PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pRasterizationState = &PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pMultisampleState = &multisample_ci,
        .pDepthStencilState = &depth_stencil_ci,
        .pColorBlendState = &PIPELINE_COLOR_BLEND_STATE_EMPTY_CREATE_INFO,
        .pDynamicState = &PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .layout = copy_stencil && !key.packed_depth_stencil
                      ? *msaa_copy_depth_stencil_pipeline_layout
                      : *msaa_copy_pipeline_layout,
        .renderPass = key.renderpass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    }));
    return *pipelines.back();
}

VkPipeline BlitImageHelper::FindOrEmplaceBlitColorMSAAPipeline(const BlitMSAAPipelineKey& key) {
    const size_t index = FindCachedKey(blit_msaa_color_keys, key, blit_msaa_color_last);
    if (index != blit_msaa_color_keys.size())
    {
        return *blit_msaa_color_pipelines[index];
    }
    blit_msaa_color_keys.push_back(key);
    VkShaderModule frag_module = *blit_color_msaa_frag;
    if (key.format_class == MSAACopyFormatClass::SignedInteger) {
        frag_module = *blit_color_msaa_sint_frag;
    } else if (key.format_class == MSAACopyFormatClass::UnsignedInteger) {
        frag_module = *blit_color_msaa_uint_frag;
    }
    auto stages = MakeStages(*full_screen_vert, frag_module);
    const std::array<s32, 3> specialization_values{
        static_cast<s32>(key.samples), static_cast<s32>(key.source_samples),
        static_cast<s32>(key.linear_filter)};
    const std::array<VkSpecializationMapEntry, 3> sample_entries{{
        {0, 0, sizeof(s32)}, {2, sizeof(s32), sizeof(s32)},
        {1, 2 * sizeof(s32), sizeof(s32)},
    }};
    const VkSpecializationInfo sample_info{
        .mapEntryCount = key.format_class == MSAACopyFormatClass::Float ? 3u : 2u,
        .pMapEntries = sample_entries.data(),
        .dataSize = sizeof(specialization_values),
        .pData = specialization_values.data(),
    };
    stages[1].pSpecializationInfo = &sample_info;
    const bool is_multisampled = key.samples != VK_SAMPLE_COUNT_1_BIT;
    const VkPipelineMultisampleStateCreateInfo multisample_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = key.samples,
        .sampleShadingEnable = is_multisampled ? VK_TRUE : VK_FALSE,
        .minSampleShading = is_multisampled ? 1.0f : 0.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    blit_msaa_color_pipelines.push_back(device.GetLogical().CreateGraphicsPipeline({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pInputAssemblyState = &PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pTessellationState = nullptr,
        .pViewportState = &PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pRasterizationState = &PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pMultisampleState = &multisample_ci,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &PIPELINE_COLOR_BLEND_STATE_GENERIC_CREATE_INFO,
        .pDynamicState = &PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .layout = *one_texture_pipeline_layout,
        .renderPass = key.renderpass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    }));
    return *blit_msaa_color_pipelines.back();
}

VkPipeline BlitImageHelper::FindOrEmplaceBlitDepthStencilMSAAPipeline(
    const BlitMSAAPipelineKey& key, bool blit_stencil) {
    auto& keys = blit_stencil ? blit_msaa_depth_stencil_keys : blit_msaa_depth_keys;
    auto& pipelines = blit_stencil ? blit_msaa_depth_stencil_pipelines : blit_msaa_depth_pipelines;
    auto & last = blit_stencil ? blit_msaa_depth_stencil_last : blit_msaa_depth_last;
    const size_t index = FindCachedKey(keys, key, last);
    if (index != keys.size())
    {
        return *pipelines[index];
    }
    keys.push_back(key);
    auto stages = MakeStages(*full_screen_vert, blit_stencil ? *blit_depth_stencil_msaa_frag
                                                             : *blit_depth_msaa_frag);
    const std::array<s32, 2> sample_values{static_cast<s32>(key.samples),
                                           static_cast<s32>(key.source_samples)};
    const std::array<VkSpecializationMapEntry, 2> sample_entries{{
        {0, 0, sizeof(s32)}, {2, sizeof(s32), sizeof(s32)},
    }};
    const VkSpecializationInfo sample_info{
        .mapEntryCount = static_cast<u32>(sample_entries.size()),
        .pMapEntries = sample_entries.data(),
        .dataSize = sizeof(sample_values),
        .pData = sample_values.data(),
    };
    stages[1].pSpecializationInfo = &sample_info;
    const bool is_multisampled = key.samples != VK_SAMPLE_COUNT_1_BIT;
    const VkPipelineMultisampleStateCreateInfo multisample_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = key.samples,
        .sampleShadingEnable = is_multisampled ? VK_TRUE : VK_FALSE,
        .minSampleShading = is_multisampled ? 1.0f : 0.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    pipelines.push_back(device.GetLogical().CreateGraphicsPipeline({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pInputAssemblyState = &PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pTessellationState = nullptr,
        .pViewportState = &PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pRasterizationState = &PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pMultisampleState = &multisample_ci,
        .pDepthStencilState = blit_stencil ? &PIPELINE_DEPTH_STENCIL_EXPORT_STATE_CREATE_INFO
                                           : &PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pColorBlendState = &PIPELINE_COLOR_BLEND_STATE_EMPTY_CREATE_INFO,
        .pDynamicState = &PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .layout = blit_stencil ? *two_textures_pipeline_layout : *one_texture_pipeline_layout,
        .renderPass = key.renderpass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    }));
    return *pipelines.back();
}

void BlitImageHelper::ConvertPipeline(vk::Pipeline& pipeline, VkRenderPass renderpass,
                                      bool is_target_depth) {
    if (pipeline) {
        return;
    }
    VkShaderModule frag_shader =
        is_target_depth ? *convert_float_to_depth_frag : *convert_depth_to_float_frag;
    const std::array stages = MakeStages(*full_screen_vert, frag_shader);
    pipeline = device.GetLogical().CreateGraphicsPipeline({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pInputAssemblyState = &PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pTessellationState = nullptr,
        .pViewportState = &PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pRasterizationState = &PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pMultisampleState = &PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pDepthStencilState = is_target_depth ? &PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO : nullptr,
        .pColorBlendState = is_target_depth ? &PIPELINE_COLOR_BLEND_STATE_EMPTY_CREATE_INFO
                                            : &PIPELINE_COLOR_BLEND_STATE_GENERIC_CREATE_INFO,
        .pDynamicState = &PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .layout = *one_texture_pipeline_layout,
        .renderPass = renderpass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    });
}

void BlitImageHelper::ConvertDepthToColorPipeline(vk::Pipeline& pipeline, VkRenderPass renderpass) {
    ConvertPipeline(pipeline, renderpass, false);
}

void BlitImageHelper::ConvertColorToDepthPipeline(vk::Pipeline& pipeline, VkRenderPass renderpass) {
    ConvertPipeline(pipeline, renderpass, true);
}

void BlitImageHelper::ConvertPipelineEx(vk::Pipeline& pipeline, VkRenderPass renderpass,
                                        vk::ShaderModule& module, bool single_texture,
                                        bool is_target_depth, bool write_stencil) {
    if (pipeline) {
        return;
    }
    const std::array stages = MakeStages(*full_screen_vert, *module);
    pipeline = device.GetLogical().CreateGraphicsPipeline({
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pInputAssemblyState = &PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pTessellationState = nullptr,
        .pViewportState = &PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pRasterizationState = &PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pMultisampleState = &PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pDepthStencilState = write_stencil
                                  ? &PIPELINE_DEPTH_STENCIL_EXPORT_STATE_CREATE_INFO
                                  : (is_target_depth ? &PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
                                                     : nullptr),
        .pColorBlendState = is_target_depth ? &PIPELINE_COLOR_BLEND_STATE_EMPTY_CREATE_INFO
                                            : &PIPELINE_COLOR_BLEND_STATE_GENERIC_CREATE_INFO,
        .pDynamicState = &PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .layout = single_texture ? *one_texture_pipeline_layout : *two_textures_pipeline_layout,
        .renderPass = renderpass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    });
}

void BlitImageHelper::ConvertPipelineColorTargetEx(vk::Pipeline& pipeline, VkRenderPass renderpass,
                                                   vk::ShaderModule& module) {
    ConvertPipelineEx(pipeline, renderpass, module, false, false);
}

void BlitImageHelper::ConvertPipelineDepthTargetEx(vk::Pipeline& pipeline, VkRenderPass renderpass,
                                                   vk::ShaderModule& module) {
    ConvertPipelineEx(pipeline, renderpass, module, true, true);
}

void BlitImageHelper::ConvertPipelineDepthStencilTargetEx(vk::Pipeline& pipeline,
                                                          VkRenderPass renderpass,
                                                          vk::ShaderModule& module) {
    // The stencil can only be written with shader stencil export.
    ConvertPipelineEx(pipeline, renderpass, module, true, true,
                      device.IsExtShaderStencilExportSupported());
}

} // namespace Vulkan
