// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <list>

#include "core/hle/service/am/am_types.h"
#include "core/hle/service/os/event.h"

namespace Core {
class System;
}

namespace Service::AM {

enum class ActivityState : u32 {
    ForegroundVisible = 0,
    ForegroundObscured = 1,
    BackgroundVisible = 2,
    BackgroundObscured = 3,
};

enum class FocusHandlingMode : u32 {
    AlwaysSuspend = 0,
    SuspendHomeSleep = 1,
    NoSuspend = 2,
};

class LifecycleManager {
public:
    explicit LifecycleManager(Core::System& system, KernelHelpers::ServiceContext& context,
                              bool is_application);
    ~LifecycleManager();

    Event& GetSystemEvent();
    Event& GetOperationModeChangedSystemEvent();

    FocusState GetAndClearFocusState();
    ActivityState GetActivityState() const { return m_activity_state; }
    bool GetExitRequested() const { return m_has_requested_exit; }

    void SetFocusState(FocusState state);
    void SetActivityState(ActivityState state) { m_activity_state = state; }
    void SetFocusHandlingMode(bool suspend);
    void SetOutOfFocusSuspendingEnabled(bool enabled);
    void SetFocusStateChangedNotificationEnabled(bool enabled);
    void SetOperationModeChangedNotificationEnabled(bool enabled);
    void SetPerformanceModeChangedNotificationEnabled(bool enabled);
    void SetResumeNotificationEnabled(bool enabled) { m_resume_notification_enabled = enabled; }

    void RequestExit();
    void RequestResumeNotification();
    void OnOperationAndPerformanceModeChanged();
    void PushUnorderedMessage(AppletMessage message);
    bool PopMessage(AppletMessage* out_message);

    bool IsRunnable() const;
    bool UpdateRequestedFocusState();
    void SignalSystemEventIfNeeded();

private:
    FocusState GetFocusStateWhileForegroundObscured() const;
    FocusState GetFocusStateWhileBackground(bool is_obscured) const;
    AppletMessage PopMessageInOrderOfPriority();
    bool ShouldSignalSystemEvent() const;

    Event m_system_event;
    Event m_operation_mode_changed_system_event;
    std::list<AppletMessage> m_unordered_messages{};

    bool m_is_application{};
    bool m_focus_state_changed_notification_enabled{true};
    bool m_operation_mode_changed_notification_enabled{true};
    bool m_performance_mode_changed_notification_enabled{true};
    bool m_resume_notification_enabled{};
    bool m_has_resume{};
    bool m_has_focus_state_changed{true};
    bool m_has_operation_mode_changed{};
    bool m_has_performance_mode_changed{};
    bool m_has_requested_exit{};
    bool m_has_acknowledged_exit{};
    bool m_applet_message_available{};

    FocusHandlingMode m_focus_handling_mode{FocusHandlingMode::SuspendHomeSleep};
    ActivityState m_activity_state{ActivityState::ForegroundVisible};
    FocusState m_requested_focus_state{};
    FocusState m_acknowledged_focus_state{};
};

} // namespace Service::AM
