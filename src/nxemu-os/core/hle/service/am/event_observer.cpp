// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/event_observer.h"
#include "core/hle/service/am/process_holder.h"
#include "core/hle/service/am/window_system.h"
#include "yuzu_common/thread.h"

namespace Service::AM {

namespace {

enum class UserDataTag : u32 {
    WakeupEvent,
    AppletProcess,
};

bool IsProcessRunning(const Kernel::KProcess& process) {
    const auto state = process.GetState();
    return state == Kernel::KProcess::State::Running ||
           state == Kernel::KProcess::State::RunningAttached ||
           state == Kernel::KProcess::State::DebugBreak;
}

} // namespace

EventObserver::EventObserver(Core::System& system, WindowSystem& window_system)
    : m_system(system), m_context(system, "am:EventObserver"), m_window_system(window_system),
      m_wakeup_event(m_context), m_wakeup_holder(m_wakeup_event.GetHandle()) {
    m_window_system.SetEventObserver(this);
    m_wakeup_holder.SetUserData(static_cast<uintptr_t>(UserDataTag::WakeupEvent));
    m_wakeup_holder.LinkToMultiWait(&m_multi_wait);

    m_thread = std::jthread([this](std::stop_token stop_token) {
        Common::SetCurrentThreadName("am:EventObserver");
        while (!stop_token.stop_requested()) {
            auto* const holder = WaitSignaled(stop_token);
            if (holder == nullptr) {
                break;
            }
            Process(holder);
        }
    });
}

EventObserver::~EventObserver() {
    m_window_system.SetEventObserver(nullptr);

    if (m_thread.joinable()) {
        m_thread.request_stop();
        m_wakeup_event.Signal();
        m_thread.join();
    }

    std::scoped_lock lk{m_lock};
    auto it = m_process_holder_list.begin();
    while (it != m_process_holder_list.end()) {
        auto* const holder = std::addressof(*it);
        it = m_process_holder_list.erase(it);
        holder->UnlinkFromMultiWait();
        delete holder;
    }
}

void EventObserver::TrackAppletProcess(std::shared_ptr<Applet> applet) {
    auto* const process = applet->lifecycle_process;
    if (process == nullptr) {
        return;
    }
    if (process->IsTerminated()) {
        RequestUpdate();
        return;
    }

    auto* const holder = new ProcessHolder(std::move(applet), *process);
    holder->SetUserData(static_cast<uintptr_t>(UserDataTag::AppletProcess));

    {
        std::scoped_lock lk{m_lock};
        m_process_holder_list.push_back(*holder);
        holder->LinkToMultiWait(&m_deferred_wait_list);
    }

    m_wakeup_event.Signal();
}

void EventObserver::RequestUpdate() {
    m_wakeup_event.Signal();
}

void EventObserver::LinkDeferred() {
    std::scoped_lock lk{m_lock};
    m_multi_wait.MoveAll(&m_deferred_wait_list);
}

MultiWaitHolder* EventObserver::WaitSignaled(std::stop_token stop_token) {
    while (true) {
        LinkDeferred();
        if (stop_token.stop_requested()) {
            return nullptr;
        }

        auto* const selected = m_multi_wait.WaitAny(m_system.Kernel());
        if (selected == nullptr) {
            if (stop_token.stop_requested()) {
                return nullptr;
            }
            continue;
        }
        if (selected != &m_wakeup_holder) {
            selected->UnlinkFromMultiWait();
        }
        return selected;
    }
}

void EventObserver::Process(MultiWaitHolder* holder) {
    switch (static_cast<UserDataTag>(holder->GetUserData())) {
    case UserDataTag::WakeupEvent:
        OnWakeupEvent();
        break;
    case UserDataTag::AppletProcess:
        OnProcessEvent(static_cast<ProcessHolder*>(holder));
        break;
    default:
        UNREACHABLE();
    }
}

void EventObserver::OnWakeupEvent() {
    m_wakeup_event.Clear();
    m_window_system.Update();
}

void EventObserver::OnProcessEvent(ProcessHolder* holder) {
    // A synchronous window update may already have unlinked this applet. Keep it alive
    // until after the applet lock is released, even when the holder is deleted below.
    const auto applet = holder->GetApplet();
    auto& process = holder->GetProcess();

    {
        std::scoped_lock lk{m_lock, applet->lock};
        if (process.IsTerminated()) {
            applet->is_running = false;
            applet->is_activity_state_applied = false;
            DestroyAppletProcessHolderLocked(holder);
        } else {
            const auto reset_result = process.Reset();
            if (reset_result.IsError() && process.IsTerminated()) {
                applet->is_running = false;
                applet->is_activity_state_applied = false;
                DestroyAppletProcessHolderLocked(holder);
            } else {
                holder->LinkToMultiWait(&m_deferred_wait_list);
                applet->is_running = IsProcessRunning(process);
                if (!applet->is_running) {
                    applet->is_activity_state_applied = false;
                }
            }
        }
    }

    m_window_system.Update();
}

void EventObserver::DestroyAppletProcessHolderLocked(ProcessHolder* holder) {
    m_process_holder_list.erase(m_process_holder_list.iterator_to(*holder));
    delete holder;
}

} // namespace Service::AM
