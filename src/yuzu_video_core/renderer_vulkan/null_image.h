// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <vulkan/vulkan.h>
#include "yuzu_shader_recompiler/shader_info.h"

namespace Vulkan::NullImage {
inline constexpr size_t Count = 3;
inline constexpr VkFormat Format = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr std::array Types{VK_IMAGE_TYPE_1D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D};
inline constexpr std::array Views{VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY,
    VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D,
    VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_IMAGE_VIEW_TYPE_MAX_ENUM,
    VK_IMAGE_VIEW_TYPE_2D};
static_assert(Views.size() == Shader::NUM_TEXTURE_TYPES);
inline constexpr size_t Index(Shader::TextureType type) {
    switch (type) {
    case Shader::TextureType::Color1D:
    case Shader::TextureType::ColorArray1D: return 0;
    case Shader::TextureType::Color3D: return 2;
    default: return 1;
    }
}
inline constexpr VkImageSubresourceRange Range(size_t index) {
    return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, index == 1 ? 6U : 1U};
}
inline constexpr VkImageCreateInfo ImageInfo(size_t index) {
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = index == 1 ? VkImageCreateFlags{VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT} : 0U,
        .imageType = Types[index], .format = Format, .extent = {1, 1, 1},
        .mipLevels = 1, .arrayLayers = Range(index).layerCount,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
}
inline constexpr VkImageViewCreateInfo ViewInfo(Shader::TextureType type, VkImage image) {
    const bool cube = type == Shader::TextureType::ColorCube ||
                      type == Shader::TextureType::ColorArrayCube;
    return {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image,
        .viewType = Views[static_cast<size_t>(type)], .format = Format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6U : 1U}};
}
template <class CommandBuffer, size_t N>
void Initialize(CommandBuffer cmdbuf, const std::array<VkImage, N>& images,
                const std::array<size_t, N>& indices) {
    std::array<VkImageMemoryBarrier, N> barriers{};
    for (size_t i = 0; i < N; ++i) {
        barriers[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i], .subresourceRange = Range(indices[i])};
    }
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, {}, {}, barriers);
    for (size_t i = 0; i < N; ++i)
        cmdbuf.ClearColorImage(images[i], VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{}, Range(indices[i]));
    const VkMemoryBarrier visible{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, visible);
}
template <class CommandBuffer>
void Initialize(CommandBuffer cmdbuf, const std::array<VkImage, Count>& images) {
    Initialize(cmdbuf, images, std::array<size_t, Count>{0, 1, 2});
}
} // namespace Vulkan::NullImage
