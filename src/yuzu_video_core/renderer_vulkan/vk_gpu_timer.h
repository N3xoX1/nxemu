// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>

#include "yuzu_common/common_types.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MasterSemaphore;

class GpuTimer {
public:
    explicit GpuTimer(const Device& device, const MasterSemaphore& master_semaphore);
    ~GpuTimer();

    void Begin(vk::CommandBuffer cmdbuf);
    void End(vk::CommandBuffer cmdbuf, u64 tick);

private:
    static constexpr u32 PAIRS = 256;

    bool Create();
    void Collect();

    const Device& device;
    const MasterSemaphore& master_semaphore;
    vk::QueryPool pool;
    bool unsupported = false;
    double period = 0.0;
    u64 mask = 0;
    std::array<u64, PAIRS> ticks{};
    u32 head = 0;
    u32 tail = 0;
    bool open = false;
    u64 last_end = 0;
};

} // namespace Vulkan
