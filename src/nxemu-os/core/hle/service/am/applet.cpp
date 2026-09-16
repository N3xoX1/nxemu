// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/scope_exit.h"

#include "core/core.h"
#include "core/hle/service/am/am_results.h"
#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/applet_manager.h"

namespace Service::AM {

Applet::Applet(Core::System& system, std::unique_ptr<Process> process_)
    : context(system, "Applet"), message_queue(system), process(std::move(process_)),
      hid_registration(system, *process), gpu_error_detected_event(context),
      friend_invitation_storage_channel_event(context), notification_storage_channel_event(context),
      health_warning_disappeared_system_event(context), acquired_sleep_lock_event(context),
      pop_from_general_channel_event(context), library_applet_launchable_event(context),
      accumulated_suspended_tick_changed_event(context), sleep_lock_event(context) {

    aruid = process->GetProcessId();
    program_id = process->GetProgramId();

    // NXEmu does not emulate the health-warning overlay, so start with its dismissal event signaled.
    health_warning_disappeared_system_event.Signal();
}

Applet::~Applet() = default;

} // namespace Service::AM
