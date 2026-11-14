// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "yuzu_video_core/gpu_time_status.h"
#include "yuzu_video_core/renderer_vulkan/vk_gpu_timer.h"
#include "yuzu_video_core/renderer_vulkan/vk_master_semaphore.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

GpuTimer::GpuTimer(const Device& device_, const MasterSemaphore& master_semaphore_)
    : device{device_}, master_semaphore{master_semaphore_} {}

GpuTimer::~GpuTimer() = default;

bool GpuTimer::Create() {
    if (pool) {
        return true;
    }
    if (unsupported) {
        return false;
    }
    unsupported = true;
    const auto families = device.GetPhysical().GetQueueFamilyProperties();
    const u32 bits = device.GetGraphicsFamily() < families.size()
                         ? families[device.GetGraphicsFamily()].timestampValidBits
                         : 0;
    period = device.GetTimestampPeriod();
    if (bits == 0 || period <= 0.0) {
        return false;
    }
    mask = bits >= 64 ? ~u64{0} : (u64{1} << bits) - 1;
    try {
        pool = device.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = PAIRS * 2,
            .pipelineStatistics = 0,
        });
    } catch (const vk::Exception&) {
        return false;
    }
    device.GetLogical().ResetQueryPool(*pool, 0, PAIRS * 2);
    unsupported = false;
    return true;
}

void GpuTimer::Collect() {
    while (head != tail && master_semaphore.IsFree(ticks[head % PAIRS])) {
        const u32 query = (head % PAIRS) * 2;
        std::array<u64, 2> values{};
        if (device.GetLogical().GetQueryResults(*pool, query, 2, sizeof(values), values.data(),
                                                sizeof(u64),
                                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const u64 start = values[0] & mask;
            const u64 end = values[1] & mask;
            if (start < last_end && last_end - start > mask / 2) {
                last_end = 0;
            }
            const u64 from = std::max(start, last_end);
            if (end > from) {
                VideoCore::gpu_time_status.Add(
                    static_cast<u64>(static_cast<double>(end - from) * period));
            }
            if (end >= start) {
                last_end = std::max(last_end, end);
            }
        }
        device.GetLogical().ResetQueryPool(*pool, query, 2);
        head++;
    }
}

void GpuTimer::Begin(vk::CommandBuffer cmdbuf) {
    Collect();
    if (!VideoCore::gpu_time_status.Measuring() || tail - head == PAIRS || !Create()) {
        return;
    }
    cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, *pool, (tail % PAIRS) * 2);
    open = true;
}

void GpuTimer::End(vk::CommandBuffer cmdbuf, u64 tick) {
    if (!open) {
        return;
    }
    open = false;
    cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *pool, (tail % PAIRS) * 2 + 1);
    ticks[tail % PAIRS] = tick;
    tail++;
}

} // namespace Vulkan
