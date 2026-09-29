// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>

#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/os/event.h"
#include "core/hle/service/os/multi_wait.h"
#include "yuzu_common/intrusive_list.h"
#include "yuzu_common/polyfill_thread.h"

namespace Core {
class System;
}

namespace Service::AM {

struct Applet;
class ProcessHolder;
class WindowSystem;

class EventObserver {
public:
    EventObserver(Core::System& system, WindowSystem& window_system);
    ~EventObserver();

    void TrackAppletProcess(std::shared_ptr<Applet> applet);
    void RequestUpdate();

private:
    void LinkDeferred();
    MultiWaitHolder* WaitSignaled(std::stop_token stop_token);
    void Process(MultiWaitHolder* holder);
    void OnWakeupEvent();
    void OnProcessEvent(ProcessHolder* holder);
    void DestroyAppletProcessHolderLocked(ProcessHolder* holder);

    Core::System& m_system;
    KernelHelpers::ServiceContext m_context;
    WindowSystem& m_window_system;

    Event m_wakeup_event;
    MultiWaitHolder m_wakeup_holder;

    std::mutex m_lock{};
    Common::IntrusiveListBaseTraits<ProcessHolder>::ListType m_process_holder_list;
    MultiWait m_multi_wait;
    MultiWait m_deferred_wait_list;
    std::jthread m_thread{};
};

} // namespace Service::AM
