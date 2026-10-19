// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "yuzu_video_core/renderer_vulkan/descriptor_payload.h"
#include "yuzu_video_core/renderer_vulkan/descriptor_copy_cache.h"

#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

struct DescriptorUpdateEntry {
    struct Empty {};

    DescriptorUpdateEntry() = default;
    DescriptorUpdateEntry(VkDescriptorImageInfo image_) : image{image_} {}
    DescriptorUpdateEntry(VkDescriptorBufferInfo buffer_) : buffer{buffer_} {}
    DescriptorUpdateEntry(VkBufferView texel_buffer_) : texel_buffer{texel_buffer_} {}

    union {
        Empty empty{};
        VkDescriptorImageInfo image;
        VkDescriptorBufferInfo buffer;
        VkBufferView texel_buffer;
    };
};

class UpdateDescriptorQueue final {
    static constexpr size_t MAX_PAYLOAD_BLOCKS = 8;
    static constexpr size_t PAYLOAD_BLOCK_SIZE = 0x20000;

public:
    explicit UpdateDescriptorQueue(const Device& device_, Scheduler& scheduler_);
    ~UpdateDescriptorQueue();

    void Acquire(size_t num_entries);

    const DescriptorUpdateEntry* UpdateData() const noexcept {
        return payload.Data();
    }

    /// Returns the entries added since the queue was acquired.
    std::span<const DescriptorUpdateEntry> Entries() const noexcept {
        return {payload.Data(), payload.Size()};
    }

    // Only the scheduler worker accesses the copy cache.
    DescriptorCopyCache<VkDescriptorSet>& CopyCache() { return copy_cache; }

    void AddSampledImage(VkImageView image_view, VkSampler sampler) {
        payload.Append(VkDescriptorImageInfo{
            .sampler = sampler,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        });
    }

    void AddImage(VkImageView image_view) {
        payload.Append(VkDescriptorImageInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = image_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        });
    }

    void AddBuffer(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
        payload.Append(VkDescriptorBufferInfo{
            .buffer = buffer,
            .offset = offset,
            .range = size,
        });
    }

    void AddTexelBuffer(VkBufferView texel_buffer) {
        payload.Append(texel_buffer);
    }

private:
    const Device& device;
    Scheduler& scheduler;

    DescriptorPayload<DescriptorUpdateEntry, PAYLOAD_BLOCK_SIZE, MAX_PAYLOAD_BLOCKS> payload;
    DescriptorCopyCache<VkDescriptorSet> copy_cache;

};

// TODO: should these be separate classes instead?
using GuestDescriptorQueue = UpdateDescriptorQueue;
using ComputePassDescriptorQueue = UpdateDescriptorQueue;

} // namespace Vulkan
