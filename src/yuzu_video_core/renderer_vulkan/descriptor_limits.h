// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <vulkan/vulkan_core.h>

namespace Vulkan {
namespace DescriptorLimitsDetail {
struct DescriptorUsage {
    uint64_t samplers{}, sampled_images{}, storage_images{}, uniform_buffers{}, storage_buffers{};
    uint64_t resources{};

    bool Add(VkDescriptorType type, uint32_t count) {
        switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            uniform_buffers += count;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            storage_buffers += count;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            sampled_images += count;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            storage_images += count;
            break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            samplers += count;
            sampled_images += count;
            break;
        default:
            return false;
        }
        // Combined image samplers count as one resource, but against both type limits.
        resources += count;
        return true;
    }
};
} // namespace DescriptorLimitsDetail

// Check the actual pipeline layout once at construction, including widened stage visibility.
// No per-draw cost, fixed stage count, and 64-bit sums for untrusted descriptor counts.
inline const char* DescriptorLimitViolation(
    const VkPhysicalDeviceLimits& limits, std::span<const VkDescriptorSetLayoutBinding> bindings,
    uint32_t color_attachments = 0) {
    constexpr std::array stages{
        VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
        VK_SHADER_STAGE_GEOMETRY_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_SHADER_STAGE_COMPUTE_BIT,
    };
    DescriptorLimitsDetail::DescriptorUsage total;
    std::array<DescriptorLimitsDetail::DescriptorUsage, stages.size()> usage{};
    for (const auto& binding : bindings) {
        if (!total.Add(binding.descriptorType, binding.descriptorCount)) {
            return "Unsupported descriptor type in pipeline layout";
        }
        for (std::size_t stage = 0; stage < stages.size(); ++stage) {
            if ((binding.stageFlags & stages[stage]) != 0) {
                usage[stage].Add(binding.descriptorType, binding.descriptorCount);
            }
        }
    }
    if (total.samplers > limits.maxDescriptorSetSamplers ||
        total.sampled_images > limits.maxDescriptorSetSampledImages ||
        total.storage_images > limits.maxDescriptorSetStorageImages ||
        total.uniform_buffers > limits.maxDescriptorSetUniformBuffers ||
        total.storage_buffers > limits.maxDescriptorSetStorageBuffers) {
        return "Pipeline exceeds descriptor set resource limits";
    }
    for (std::size_t stage = 0; stage < stages.size(); ++stage) {
        const auto& count = usage[stage];
        const uint64_t attachments = stages[stage] == VK_SHADER_STAGE_FRAGMENT_BIT
                                         ? color_attachments : 0;
        if (count.samplers > limits.maxPerStageDescriptorSamplers ||
            count.sampled_images > limits.maxPerStageDescriptorSampledImages ||
            count.storage_images > limits.maxPerStageDescriptorStorageImages ||
            count.uniform_buffers > limits.maxPerStageDescriptorUniformBuffers ||
            count.storage_buffers > limits.maxPerStageDescriptorStorageBuffers ||
            count.resources + attachments > limits.maxPerStageResources) {
            return "Pipeline exceeds per-stage resource limits";
        }
        if (stages[stage] == VK_SHADER_STAGE_FRAGMENT_BIT &&
            attachments + count.storage_images + count.storage_buffers >
                limits.maxFragmentCombinedOutputResources) {
            return "Pipeline exceeds combined fragment output resource limit";
        }
    }
    return nullptr;
}
} // namespace Vulkan
