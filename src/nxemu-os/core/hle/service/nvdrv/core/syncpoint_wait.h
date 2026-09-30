// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>

#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "nxemu-module-spec/video.h"

namespace Service::Nvidia::NvCore {

// Wakes the server to retry an IPC request; never waits on its service thread.
// The request owns this object until reply or session teardown.
class SyncpointWait final {
public:
    SyncpointWait(IVideo& video_, Core::Timing::CoreTiming& timing_, Kernel::KEvent& event_,
                  u32 id_, u32 threshold, u32 timeout_ms)
        : video{video_}, timing{timing_}, event{event_}, id{id_} {
        action = video.HostSyncpointRegisterAction(id, threshold, OnSyncpoint, 0, this);
        // UINT32_MAX is the infinite-wait sentinel. Infinite waits need no timer or worker.
        try {
            if (timeout_ms != UINT32_MAX && !IsSignalled()) {
                timeout = Core::Timing::CreateEvent(
                    "NVDRV::SyncpointWait",
                    [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                        Signal();
                        return std::nullopt;
                    });
                timing.ScheduleEvent(std::chrono::milliseconds{timeout_ms}, timeout);
            }
        } catch (...) {
            // A failed timer allocation must not leave a callback pointing to this constructor.
            if (action != 0) {
                video.DeregisterHostAction(id, action);
            }
            throw;
        }
    }

    ~SyncpointWait() {
        // Deregistration synchronizes with Host1x dispatch. UnscheduleEvent waits for any
        // running timer callback. Neither callback can touch this object after destruction.
        if (action != 0) {
            video.DeregisterHostAction(id, action);
        }
        if (timeout) {
            timing.UnscheduleEvent(timeout);
        }
    }

    SyncpointWait(const SyncpointWait&) = delete;
    SyncpointWait& operator=(const SyncpointWait&) = delete;

    bool IsSignalled() const {
        return signalled.load(std::memory_order_acquire);
    }

private:
    static void OnSyncpoint(u32, void* user_data) {
        static_cast<SyncpointWait*>(user_data)->Signal();
    }

    void Signal() {
        if (!signalled.exchange(true, std::memory_order_acq_rel)) {
            event.Signal();
        }
    }

    IVideo& video;
    Core::Timing::CoreTiming& timing;
    Kernel::KEvent& event;
    u32 id;
    u32 action{};
    std::shared_ptr<Core::Timing::EventType> timeout;
    std::atomic_bool signalled{};
};

} // namespace Service::Nvidia::NvCore
