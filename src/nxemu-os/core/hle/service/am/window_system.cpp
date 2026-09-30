// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <utility>

#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_data_broker.h"
#include "core/hle/service/am/event_observer.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/am/window_system.h"

namespace Service::AM {

WindowSystem::WindowSystem(Core::System& system) : m_system(system) {}
WindowSystem::~WindowSystem() = default;

void WindowSystem::SetEventObserver(EventObserver* event_observer) {
    std::scoped_lock lk{m_lock};
    m_event_observer = event_observer;
}

void WindowSystem::TrackApplet(std::shared_ptr<Applet> applet, bool is_application) {
    {
        std::scoped_lock lk{m_lock};

        // Frontend applets (ARUID 0) are owned by their caller, not by the process map.
        if (applet->aruid == 0 || !m_applets.emplace(applet->aruid, applet).second) {
            return;
        }

        if (applet->applet_id == AppletId::QLaunch) {
            m_home_menu = applet.get();
        } else if (applet->applet_id == AppletId::OverlayDisplay) {
            m_overlay_display = applet.get();
        } else if (is_application) {
            m_application = applet.get();
        }

        // The observer detaches under this lock before it stops accepting processes.
        if (m_event_observer != nullptr) {
            m_event_observer->TrackAppletProcess(applet);
        }
    }

    Update();
}

void WindowSystem::Reset() {
    std::scoped_lock lk{m_lock};
    m_applets.clear();
    m_home_menu = nullptr;
    m_application = nullptr;
    m_overlay_display = nullptr;
    m_foreground_requested_applet = nullptr;
}

std::shared_ptr<Applet> WindowSystem::GetByAppletResourceUserId(AppletResourceUserId aruid) const {
    std::scoped_lock lk{m_lock};
    const auto it = m_applets.find(aruid);
    if (it == m_applets.end()) {
        return {};
    }
    return it->second;
}

std::shared_ptr<Applet> WindowSystem::GetApplicationApplet() const {
    std::scoped_lock lk{m_lock};
    if (m_application == nullptr) {
        return {};
    }
    const auto it = m_applets.find(m_application->aruid);
    return it != m_applets.end() ? it->second : nullptr;
}

void WindowSystem::RequestApplicationToGetForeground() {
    {
        std::scoped_lock lk{m_lock};
        m_foreground_requested_applet = m_application;
    }
    Update();
}

void WindowSystem::RequestHomeMenuToGetForeground() {
    {
        std::scoped_lock lk{m_lock};
        m_foreground_requested_applet = m_home_menu;
    }
    Update();
}

void WindowSystem::RequestAppletVisibilityState(Applet& applet, bool visible) {
    {
        std::scoped_lock lk{applet.lock};
        applet.window_visible = visible;
    }
    Update();
}

void WindowSystem::NotifyAppletStarted(Applet& applet) {
    {
        std::scoped_lock lk{applet.lock};
        const auto* process = applet.lifecycle_process;
        applet.is_running = process == nullptr ||
                            process->GetState() == Kernel::KProcess::State::Running ||
                            process->GetState() == Kernel::KProcess::State::RunningAttached ||
                            process->GetState() == Kernel::KProcess::State::DebugBreak;
        applet.is_activity_state_applied = false;
    }
    Update();
}

void WindowSystem::NotifyAppletStopped(Applet& applet) {
    {
        std::scoped_lock lk{applet.lock};
        applet.is_running = false;
        applet.is_activity_state_applied = false;
        applet.SetInteractibleLocked(false, false);
        applet.display_layer_manager.SetWindowVisibility(false);
    }
    Update();
}

void WindowSystem::NotifyAppletStateChanged() {
    Update();
}

void WindowSystem::OnExitRequested() {
    {
        std::scoped_lock lk{m_lock};
        for (const auto& [aruid, applet] : m_applets) {
            std::scoped_lock applet_lk{applet->lock};
            applet->lifecycle_manager.RequestExit();
            applet->SetInteractibleLocked(applet->is_pad_interactible,
                                          applet->is_touch_interactible);
            applet->UpdateSuspensionStateLocked(true);
        }
    }
}

void WindowSystem::OnResumeRequested() {
    std::scoped_lock lk{m_lock};
    for (const auto& [aruid, applet] : m_applets) {
        std::scoped_lock applet_lk{applet->lock};
        applet->lifecycle_manager.PushUnorderedMessage(AppletMessage::Resume);
    }
}

void WindowSystem::OnOperationModeChanged() {
    std::scoped_lock lk{m_lock};
    for (const auto& [aruid, applet] : m_applets) {
        std::scoped_lock applet_lk{applet->lock};
        applet->lifecycle_manager.OnOperationAndPerformanceModeChanged();
    }
}

bool WindowSystem::PruneTerminatedAppletsLocked(
    std::vector<std::shared_ptr<AppletDataBroker>>& completed_brokers,
    std::vector<std::shared_ptr<Applet>>& children_to_terminate) {
    bool removed_any = false;

    for (auto it = m_applets.begin(); it != m_applets.end();) {
        const auto applet = it->second;
        auto* const process = applet->lifecycle_process;
        if (process == nullptr || !process->IsTerminated()) {
            ++it;
            continue;
        }

        std::weak_ptr<Applet> caller_weak;
        std::shared_ptr<AppletDataBroker> broker;
        {
            std::scoped_lock applet_lk{applet->lock};
            applet->is_running = false;
            applet->is_activity_state_applied = false;
            applet->SetInteractibleLocked(false, false);
            applet->display_layer_manager.SetWindowVisibility(false);

            if (!applet->child_applets.empty()) {
                for (const auto& child : applet->child_applets) {
                    if (std::find(children_to_terminate.begin(), children_to_terminate.end(), child) ==
                        children_to_terminate.end()) {
                        children_to_terminate.push_back(child);
                    }
                }
                ++it;
                continue;
            }

            // IPC identity getters also read this weak link. Leave it immutable and
            // detach only the caller's owning child list below.
            caller_weak = applet->caller_applet;
            broker = applet->caller_applet_broker;
        }

        if (auto caller = caller_weak.lock(); caller) {
            std::scoped_lock caller_lk{caller->lock};
            caller->child_applets.remove(applet);
        }

        if (m_foreground_requested_applet == applet.get()) {
            m_foreground_requested_applet = nullptr;
        }
        if (m_home_menu == applet.get()) {
            m_home_menu = nullptr;
            m_foreground_requested_applet = m_application;
        }
        if (m_application == applet.get()) {
            m_application = nullptr;
            m_foreground_requested_applet = m_home_menu;
            if (m_home_menu != nullptr) {
                std::scoped_lock home_lk{m_home_menu->lock};
                m_home_menu->lifecycle_manager.PushUnorderedMessage(AppletMessage::ApplicationExited);
            }
        }
        if (m_overlay_display == applet.get()) {
            m_overlay_display = nullptr;
        }

        if (broker) {
            completed_brokers.push_back(std::move(broker));
        }

        it = m_applets.erase(it);
        removed_any = true;
    }

    if (m_foreground_requested_applet == nullptr) {
        m_foreground_requested_applet = m_application != nullptr ? m_application : m_home_menu;
    }

    return removed_any;
}

void WindowSystem::Update() {
    std::vector<std::shared_ptr<AppletDataBroker>> completed_brokers;
    std::vector<std::shared_ptr<Applet>> children_to_terminate;
    bool should_exit = false;

    {
        std::scoped_lock lk{m_lock};

        bool removed_any = false;
        bool removed_in_pass = false;
        do {
            removed_in_pass =
                PruneTerminatedAppletsLocked(completed_brokers, children_to_terminate);
            removed_any = removed_any || removed_in_pass;
        } while (removed_in_pass);

        bool overlay_takes_input = false;
        if (m_overlay_display != nullptr) {
            std::scoped_lock overlay_lk{m_overlay_display->lock};
            overlay_takes_input = m_overlay_display->is_running &&
                                  m_overlay_display->window_visible &&
                                  m_overlay_display->overlay_watching_short_home_button;
        }
        UpdateAppletStateLocked(m_overlay_display, true, overlay_takes_input);
        UpdateAppletStateLocked(m_home_menu, m_foreground_requested_applet == m_home_menu,
                                overlay_takes_input);
        UpdateAppletStateLocked(m_application, m_foreground_requested_applet == m_application,
                                overlay_takes_input);

        should_exit = removed_any && m_applets.empty();
    }

    for (const auto& child : children_to_terminate) {
        // A later pruning pass may already have collected a normally exited child.
        if (child->lifecycle_process != nullptr && child->lifecycle_process->IsTerminated()) {
            continue;
        }
        if (child->caller_applet_broker->IsCompleted()) {
            continue;
        }
        {
            std::scoped_lock child_lk{child->lock};
            child->terminate_result = AM::ResultLibraryAppletTerminated;
        }
        if (child->lifecycle_process != nullptr && !child->lifecycle_process->IsTerminated()) {
            child->lifecycle_process->Terminate();
        }
        if (child->frontend) {
            child->frontend->RequestExit();
            // HLE applets have no process event, and RequestExit may only close the UI.
            child->caller_applet_broker->SignalCompletion();
        }
    }

    for (const auto& broker : completed_brokers) {
        broker->SignalCompletion(false);
    }

    if (should_exit) {
        m_system.Exit();
    }
}

void WindowSystem::UpdateAppletStateLocked(Applet* applet, bool is_foreground,
                                          bool overlay_takes_input) {
    if (applet == nullptr) {
        return;
    }

    std::scoped_lock lk{applet->lock};

    const bool has_obscuring_child_applets = [&] {
        for (const auto& child : applet->child_applets) {
            std::scoped_lock child_lk{child->lock};
            const auto mode = child->library_applet_mode;
            if (child->is_running && child->window_visible &&
                (mode == LibraryAppletMode::AllForeground ||
                 mode == LibraryAppletMode::AllForegroundInitiallyHidden)) {
                return true;
            }
        }
        return false;
    }();

    const bool inherited_foreground = applet->is_running && is_foreground;
    const auto visible_state = inherited_foreground ? ActivityState::ForegroundVisible
                                                    : ActivityState::BackgroundVisible;
    const auto obscured_state = inherited_foreground ? ActivityState::ForegroundObscured
                                                     : ActivityState::BackgroundObscured;

    applet->display_layer_manager.SetWindowVisibility(inherited_foreground && applet->window_visible);
    const bool is_overlay = applet->applet_id == AppletId::OverlayDisplay;
    const bool needs_input = inherited_foreground && applet->window_visible &&
                             (is_overlay ? applet->overlay_watching_short_home_button
                                         : !overlay_takes_input);
    applet->SetInteractibleLocked(needs_input, needs_input);

    const bool is_obscured = has_obscuring_child_applets || !applet->window_visible;
    const auto desired_state = is_obscured ? obscured_state : visible_state;
    if (applet->lifecycle_manager.GetActivityState() != desired_state) {
        applet->lifecycle_manager.SetActivityState(desired_state);
        applet->UpdateSuspensionStateLocked(true);
    } else {
        applet->UpdateSuspensionStateLocked(false);
    }

    for (const auto& child : applet->child_applets) {
        UpdateAppletStateLocked(child.get(), inherited_foreground, overlay_takes_input);
    }
}

} // namespace Service::AM
