// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "yuzu_common/common_types.h"
#include "yuzu_common/polyfill_thread.h"
#include "yuzu_common/thread.h"

#include "core/hle/service/vi/vsync_manager.h"

namespace Core {
class System;
}

namespace Core::Timing {
struct EventType;
}

namespace Service {
class Event;
}

namespace Service::VI {

class Container;
class DisplayList;

class Conductor {
public:
    explicit Conductor(Core::System& system, Container& container, DisplayList& displays);
    ~Conductor();

    void LinkVsyncEvent(u64 display_id, Event* event);
    void UnlinkVsyncEvent(u64 display_id, Event* event);

private:
    void ProcessVsync();
    void VsyncThread(std::stop_token token);
    s64 GetNextTicks() const;

private:
    Core::System& m_system;
    Container& m_container;
    std::unordered_map<u64, VsyncManager> m_vsync_managers;
    std::mutex m_vsync_mutex;
    std::shared_ptr<Core::Timing::EventType> m_event;
    Common::Event m_signal;
    std::jthread m_thread;

private:
    std::atomic<s32> m_swap_interval{1};
    std::atomic<f32> m_compose_speed_scale{1.0f};
};

} // namespace Service::VI
