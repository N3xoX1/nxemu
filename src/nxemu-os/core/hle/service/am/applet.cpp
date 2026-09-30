// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/logging/log.h"

#include "core/core.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/kernel/k_process.h"

namespace Service::AM {

Applet::Applet(Core::System& system, std::unique_ptr<Process> process_, bool is_application,
               Kernel::KProcess* lifecycle_process_)
    : context(system, "Applet"), lifecycle_manager(system, context, is_application),
      process(std::move(process_)), lifecycle_process(lifecycle_process_),
      hid_registration(system), gpu_error_detected_event(context),
      friend_invitation_storage_channel_event(context), notification_storage_channel_event(context),
      health_warning_disappeared_system_event(context),
      hdcp_authentication_state_changed_event(context), unknown_event(context),
      acquired_sleep_lock_event(context),
      pop_from_general_channel_event(context), library_applet_launchable_event(context),
      accumulated_suspended_tick_changed_event(context), sleep_lock_event(context) {

    aruid = process->GetProcessId();
    program_id = process->GetProgramId();
    hid_registration.SetAppletResourceUserId(aruid);
    if (lifecycle_process == nullptr) {
        lifecycle_process = process->GetProcess();
    }

    // NXEmu does not emulate the health-warning overlay, so start with its dismissal event signaled.
    health_warning_disappeared_system_event.Signal();
}

Applet::~Applet() = default;

void Applet::SetAppletResourceUserId(AppletResourceUserId new_aruid) {
    aruid = new_aruid;
    hid_registration.SetAppletResourceUserId(aruid);
}

void Applet::SetInteractibleLocked(bool pad_interactible, bool touch_interactible) {
    is_pad_interactible = pad_interactible;
    is_touch_interactible = touch_interactible;

    const bool exit_requested = lifecycle_manager.GetExitRequested();
    hid_registration.EnableAppletToGetInput(pad_interactible && !exit_requested,
                                             touch_interactible && !exit_requested);
}

void Applet::UpdateSuspensionStateLocked(bool force_message) {
    const bool runnable = lifecycle_manager.IsRunnable();
    const bool changed = runnable != is_activity_runnable;

    if ((changed || !is_activity_state_applied) && is_running && lifecycle_process != nullptr) {
        is_activity_state_applied = false;
        const auto activity = runnable ? Kernel::Svc::ProcessActivity::Runnable
                                       : Kernel::Svc::ProcessActivity::Paused;
        if (lifecycle_process->IsSuspended() != !runnable) {
            const auto result = lifecycle_process->SetActivity(activity);
            if (result.IsError()) {
                LOG_WARNING(Service_AM, "Failed to update applet process activity, result={:#x}",
                            result.raw);
            } else {
                is_activity_state_applied = true;
            }
        } else {
            is_activity_state_applied = true;
        }
    }

    if (changed && !runnable) {
        lifecycle_manager.RequestResumeNotification();
    }

    is_activity_runnable = runnable;
    if (lifecycle_manager.UpdateRequestedFocusState() || changed || force_message) {
        lifecycle_manager.SignalSystemEventIfNeeded();
    }
}

} // namespace Service::AM
