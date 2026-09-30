// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "yuzu_common/common_types.h"

namespace Core {
class System;
}

namespace Service::HID {
class IHidServer;
}

namespace Service::AM {

class HidRegistration {
public:
    explicit HidRegistration(Core::System& system);
    ~HidRegistration();

    // After publication, the owning applet's lock serializes registration and input updates.
    void SetAppletResourceUserId(u64 aruid);
    bool EnsureRegistered();
    void EnableAppletToGetInput(bool enable_pad, bool enable_touch);

private:
    void ApplyInputState();
    void Unregister();

    Core::System& m_system;
    u64 m_aruid{};
    bool m_registered{};
    bool m_input_state_set{};
    bool m_input_state_applied{};
    bool m_pad_enabled{};
    bool m_touch_enabled{};
    std::shared_ptr<Service::HID::IHidServer> m_hid_server;
};

} // namespace Service::AM
