// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <variant>
#include <boost/container/static_vector.hpp>

#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

UpdateDescriptorQueue::UpdateDescriptorQueue(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {}

UpdateDescriptorQueue::~UpdateDescriptorQueue() = default;

void UpdateDescriptorQueue::TickFrame() {
    payload.TickFrame();
}

void UpdateDescriptorQueue::Acquire(size_t num_entries) {
    payload.Acquire(num_entries, [&] {
        LOG_DEBUG(Render_Vulkan, "Recycling descriptor payload after waiting for worker");
        scheduler.WaitWorker();
    });
}

} // namespace Vulkan
