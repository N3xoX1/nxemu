// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/yuzu_assert.h"
#include "core/hle/service/am/lifecycle_manager.h"

namespace Service::AM {

LifecycleManager::LifecycleManager(Core::System&, KernelHelpers::ServiceContext& context,
                                   bool is_application)
    : m_system_event(context), m_operation_mode_changed_system_event(context),
      m_is_application(is_application) {}

LifecycleManager::~LifecycleManager() = default;

Event& LifecycleManager::GetSystemEvent() {
    return m_system_event;
}

Event& LifecycleManager::GetOperationModeChangedSystemEvent() {
    return m_operation_mode_changed_system_event;
}

FocusState LifecycleManager::GetAndClearFocusState() {
    m_acknowledged_focus_state = m_requested_focus_state;
    return m_acknowledged_focus_state;
}

void LifecycleManager::SetFocusState(FocusState state) {
    if (m_requested_focus_state != state) {
        m_has_focus_state_changed = true;
    }
    m_requested_focus_state = state;
    SignalSystemEventIfNeeded();
}

void LifecycleManager::SetFocusStateChangedNotificationEnabled(bool enabled) {
    m_focus_state_changed_notification_enabled = enabled;
    SignalSystemEventIfNeeded();
}

void LifecycleManager::SetOperationModeChangedNotificationEnabled(bool enabled) {
    m_operation_mode_changed_notification_enabled = enabled;
    SignalSystemEventIfNeeded();
}

void LifecycleManager::SetPerformanceModeChangedNotificationEnabled(bool enabled) {
    m_performance_mode_changed_notification_enabled = enabled;
    SignalSystemEventIfNeeded();
}

void LifecycleManager::RequestExit() {
    m_has_requested_exit = true;
    SignalSystemEventIfNeeded();
}

void LifecycleManager::RequestResumeNotification() {
    if (m_resume_notification_enabled) {
        m_has_resume = true;
    }
}

void LifecycleManager::OnOperationAndPerformanceModeChanged() {
    if (m_operation_mode_changed_notification_enabled) {
        m_has_operation_mode_changed = true;
    }
    if (m_performance_mode_changed_notification_enabled) {
        m_has_performance_mode_changed = true;
    }
    m_operation_mode_changed_system_event.Signal();
    SignalSystemEventIfNeeded();
}

void LifecycleManager::PushUnorderedMessage(AppletMessage message) {
    m_unordered_messages.push_back(message);
    SignalSystemEventIfNeeded();
}

AppletMessage LifecycleManager::PopMessageInOrderOfPriority() {
    if (m_has_resume) {
        m_has_resume = false;
        return AppletMessage::Resume;
    }
    if (m_has_acknowledged_exit != m_has_requested_exit) {
        m_has_acknowledged_exit = m_has_requested_exit;
        return AppletMessage::Exit;
    }
    if (m_focus_state_changed_notification_enabled) {
        if (!m_is_application) {
            if (m_requested_focus_state != m_acknowledged_focus_state) {
                m_acknowledged_focus_state = m_requested_focus_state;
                if (m_requested_focus_state == FocusState::InFocus) {
                    return AppletMessage::ChangeIntoForeground;
                }
                return AppletMessage::ChangeIntoBackground;
            }
        } else if (m_has_focus_state_changed) {
            m_has_focus_state_changed = false;
            return AppletMessage::FocusStateChanged;
        }
    }
    if (m_has_operation_mode_changed) {
        m_has_operation_mode_changed = false;
        return AppletMessage::OperationModeChanged;
    }
    if (m_has_performance_mode_changed) {
        m_has_performance_mode_changed = false;
        return AppletMessage::PerformanceModeChanged;
    }
    if (!m_unordered_messages.empty()) {
        const auto message = m_unordered_messages.front();
        m_unordered_messages.pop_front();
        return message;
    }
    return AppletMessage::None;
}

bool LifecycleManager::ShouldSignalSystemEvent() const {
    if (m_focus_state_changed_notification_enabled) {
        if (!m_is_application) {
            if (m_requested_focus_state != m_acknowledged_focus_state) {
                return true;
            }
        } else if (m_has_focus_state_changed) {
            return true;
        }
    }
    return !m_unordered_messages.empty() || m_has_resume ||
           (m_has_requested_exit != m_has_acknowledged_exit) ||
           m_has_operation_mode_changed || m_has_performance_mode_changed;
}

void LifecycleManager::SignalSystemEventIfNeeded() {
    const bool available = ShouldSignalSystemEvent();
    if (available == m_applet_message_available) {
        return;
    }
    m_applet_message_available = available;
    if (available) {
        m_system_event.Signal();
    } else {
        m_system_event.Clear();
    }
}

bool LifecycleManager::PopMessage(AppletMessage* out_message) {
    *out_message = PopMessageInOrderOfPriority();
    SignalSystemEventIfNeeded();
    return *out_message != AppletMessage::None;
}

void LifecycleManager::SetFocusHandlingMode(bool suspend) {
    switch (m_focus_handling_mode) {
    case FocusHandlingMode::AlwaysSuspend:
    case FocusHandlingMode::SuspendHomeSleep:
        if (!suspend) {
            m_focus_handling_mode = FocusHandlingMode::NoSuspend;
        }
        break;
    case FocusHandlingMode::NoSuspend:
        if (suspend) {
            m_focus_handling_mode = FocusHandlingMode::SuspendHomeSleep;
        }
        break;
    }
}

void LifecycleManager::SetOutOfFocusSuspendingEnabled(bool enabled) {
    switch (m_focus_handling_mode) {
    case FocusHandlingMode::AlwaysSuspend:
        if (!enabled) {
            m_focus_handling_mode = FocusHandlingMode::SuspendHomeSleep;
        }
        break;
    case FocusHandlingMode::SuspendHomeSleep:
    case FocusHandlingMode::NoSuspend:
        if (enabled) {
            m_focus_handling_mode = FocusHandlingMode::AlwaysSuspend;
        }
        break;
    }
}

bool LifecycleManager::IsRunnable() const {
    if (m_has_requested_exit) {
        return true;
    }
    if (m_activity_state == ActivityState::ForegroundVisible) {
        return true;
    }
    if (m_activity_state == ActivityState::ForegroundObscured) {
        return m_focus_handling_mode != FocusHandlingMode::AlwaysSuspend;
    }
    return m_focus_handling_mode == FocusHandlingMode::NoSuspend;
}

FocusState LifecycleManager::GetFocusStateWhileForegroundObscured() const {
    switch (m_focus_handling_mode) {
    case FocusHandlingMode::AlwaysSuspend:
        return FocusState::InFocus;
    case FocusHandlingMode::SuspendHomeSleep:
    case FocusHandlingMode::NoSuspend:
        return FocusState::NotInFocus;
    }
    UNREACHABLE();
}

FocusState LifecycleManager::GetFocusStateWhileBackground(bool is_obscured) const {
    switch (m_focus_handling_mode) {
    case FocusHandlingMode::AlwaysSuspend:
        return FocusState::InFocus;
    case FocusHandlingMode::SuspendHomeSleep:
        return is_obscured ? FocusState::NotInFocus : FocusState::InFocus;
    case FocusHandlingMode::NoSuspend:
        return m_is_application ? FocusState::Background : FocusState::NotInFocus;
    }
    UNREACHABLE();
}

bool LifecycleManager::UpdateRequestedFocusState() {
    FocusState new_state{};
    switch (m_activity_state) {
    case ActivityState::ForegroundVisible:
        new_state = FocusState::InFocus;
        break;
    case ActivityState::ForegroundObscured:
        new_state = GetFocusStateWhileForegroundObscured();
        break;
    case ActivityState::BackgroundVisible:
        new_state = GetFocusStateWhileBackground(false);
        break;
    case ActivityState::BackgroundObscured:
        new_state = GetFocusStateWhileBackground(true);
        break;
    default:
        UNREACHABLE();
    }
    if (new_state == m_requested_focus_state) {
        return false;
    }
    m_requested_focus_state = new_state;
    m_has_focus_state_changed = true;
    return true;
}

} // namespace Service::AM
