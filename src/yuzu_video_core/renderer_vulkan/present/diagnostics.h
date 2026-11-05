// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "yuzu_video_core/fsr_diagnostics.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include <array>
#include <limits>

namespace Vulkan {

class PresentTimer {
  public:
    PresentTimer(const Device &device_, Scheduler &scheduler_)
        : device(device_), scheduler(scheduler_) {
        const auto families = device.GetPhysical().GetQueueFamilyProperties();
        bits = families[device.GetGraphicsFamily()].timestampValidBits;
        period = device.GetPhysical().GetProperties().limits.timestampPeriod;
        if (bits != 0) {
            pool = device.GetLogical().CreateQueryPool(VkQueryPoolCreateInfo{
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = static_cast<u32>(slots.size() * 2),
            });
        }
    }

    ~PresentTimer() {
        // Only diagnostics teardown waits. Normal sampling never stalls the GPU.
        scheduler.Finish();
        Collect();
    }

    void Begin() {
        Collect();
        active = slots.size();
        if (!pool)
            return;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (!slots[i].pending) {
                active = i;
                const u32 first = static_cast<u32>(i * 2);
                device.GetLogical().ResetQueryPool(*pool, first, 2);
                scheduler.RequestOutsideRenderPassOperationContext();
                scheduler.Record([query_pool = *pool, first](vk::CommandBuffer cmd) {
                    cmd.WriteTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, query_pool, first);
                });
                break;
            }
        }
    }

    void End(VideoCore::PresentDiagnostics::Sample sample) {
        const auto now = VideoCore::PresentDiagnostics::Clock::now();
        sample.frame_interval_ms = previous_frame.time_since_epoch().count() == 0 ? 0.0 :
            std::chrono::duration<double, std::milli>(now - previous_frame).count();
        previous_frame = now;
        if (active == slots.size()) {
            VideoCore::PresentDiagnostics::Write("Vulkan", sample,
                                                 std::numeric_limits<double>::quiet_NaN());
            return;
        }
        auto &slot = slots[active];
        slot.sample = sample;
        slot.tick = scheduler.CurrentTick();
        slot.pending = true;
        const u32 last = static_cast<u32>(active * 2 + 1);
        scheduler.Record([query_pool = *pool, last](vk::CommandBuffer cmd) {
            cmd.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, last);
        });
    }

  private:
    void Collect() {
        scheduler.GetMasterSemaphore().Refresh();
        for (size_t i = 0; i < slots.size(); ++i) {
            auto &slot = slots[i];
            if (!slot.pending || !scheduler.IsFree(slot.tick))
                continue;
            std::array<u64, 2> times{};
            const auto result = device.GetLogical().GetQueryResults(
                *pool, static_cast<u32>(i * 2), 2, sizeof(times), times.data(), sizeof(u64),
                VK_QUERY_RESULT_64_BIT);
            if (result != VK_SUCCESS)
                continue;
            const u64 mask = bits == 64 ? ~u64{0} : (u64{1} << bits) - 1;
            const double ms = static_cast<double>((times[1] - times[0]) & mask) * period / 1.0e6;
            VideoCore::PresentDiagnostics::Write("Vulkan", slot.sample, ms);
            slot.pending = false;
        }
    }

    struct Slot {
        VideoCore::PresentDiagnostics::Sample sample;
        u64 tick{};
        bool pending{};
    };
    const Device &device;
    Scheduler &scheduler;
    vk::QueryPool pool;
    std::array<Slot, 16> slots{};
    size_t active{slots.size()};
    u32 bits{};
    double period{};
    VideoCore::PresentDiagnostics::Clock::time_point previous_frame;
};

} // namespace Vulkan
