// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/am/hid_registration.h"
#include "yuzu_common/logging/log.h"
#include "core/core.h"
#include "core/hle/service/hid/hid_server.h"
#include "yuzu_hid_core/hid_result.h"
#include "core/hle/service/sm/sm.h"
#include "yuzu_hid_core/resource_manager.h"

namespace Service::AM {

HidRegistration::HidRegistration(Core::System& system) : m_system(system) {}

HidRegistration::~HidRegistration() {
    Unregister();
}

void HidRegistration::SetAppletResourceUserId(u64 aruid) {
    if (m_aruid == aruid) {
        EnsureRegistered();
        return;
    }

    Unregister();
    m_aruid = aruid;
    EnsureRegistered();
}

bool HidRegistration::EnsureRegistered() {
    if (m_registered) {
        ApplyInputState();
        return true;
    }
    if (m_aruid == 0) {
        return false;
    }

    if (!m_hid_server) {
        m_hid_server = m_system.ServiceManager().GetService<HID::IHidServer>("hid");
    }
    if (!m_hid_server) {
        return false;
    }

    auto resource_manager = m_hid_server->GetResourceManager();
    const auto result = resource_manager->RegisterAppletResourceUserId(m_aruid, true);
    if (result.IsError() && result != HID::ResultAruidAlreadyRegistered) {
        LOG_WARNING(Service_AM, "Failed to register HID ARUID {}, result={:#x}", m_aruid,
                    result.raw);
        return false;
    }

    resource_manager->SetAruidValidForVibration(m_aruid, true);
    m_registered = true;
    m_input_state_applied = false;
    ApplyInputState();
    return true;
}

void HidRegistration::Unregister() {
    if (!m_registered || !m_hid_server) {
        m_registered = false;
        return;
    }

    auto resource_manager = m_hid_server->GetResourceManager();
    resource_manager->SetAruidValidForVibration(m_aruid, false);
    resource_manager->UnregisterAppletResourceUserId(m_aruid);
    m_registered = false;
    m_input_state_applied = false;
}

void HidRegistration::ApplyInputState() {
    if (!m_registered || !m_hid_server || !m_input_state_set) {
        return;
    }

    auto resource_manager = m_hid_server->GetResourceManager();
    if (!m_input_state_applied) {
        resource_manager->EnablePadInput(m_aruid, m_pad_enabled);
        resource_manager->EnableTouchScreen(m_aruid, m_touch_enabled);
        m_input_state_applied = true;
    }
    // NXEmu tracks one active vibration ARUID. Reassert it during window traversal so
    // the caller regains vibration after its child exits, even if its pad state is unchanged.
    resource_manager->SetAruidValidForVibration(m_aruid, m_pad_enabled);
}

void HidRegistration::EnableAppletToGetInput(bool enable_pad, bool enable_touch) {
    if (!m_input_state_set || m_pad_enabled != enable_pad || m_touch_enabled != enable_touch) {
        m_pad_enabled = enable_pad;
        m_touch_enabled = enable_touch;
        m_input_state_set = true;
        m_input_state_applied = false;
    }

    EnsureRegistered();
}

} // namespace Service::AM
