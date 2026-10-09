// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_video_core/renderer_vulkan/descriptor_payload.h"

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
    // This should be plenty for the vast majority of cases. Most desktop platforms only
    // provide up to 3 swapchain images.
    static constexpr size_t FRAMES_IN_FLIGHT = 8;
    static constexpr size_t FRAME_PAYLOAD_SIZE = 0x20000;

public:
    explicit UpdateDescriptorQueue(const Device& device_, Scheduler& scheduler_);
    ~UpdateDescriptorQueue();

    void TickFrame();

    void Acquire(size_t num_entries);

    const DescriptorUpdateEntry* UpdateData() const noexcept {
        return payload.Data();
    }

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

    DescriptorPayload<DescriptorUpdateEntry, FRAME_PAYLOAD_SIZE, FRAMES_IN_FLIGHT> payload;

};

// TODO: should these be separate classes instead?
using GuestDescriptorQueue = UpdateDescriptorQueue;
using ComputePassDescriptorQueue = UpdateDescriptorQueue;

} // namespace Vulkan
