// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <nxemu-module-spec/operating_system.h>

namespace Core::HID
{
class HIDCore;
}

class DefaultControllerApplet final : 
    public IControllerApplet
{
public:
    explicit DefaultControllerApplet(Core::HID::HIDCore & hid_core_);
    ~DefaultControllerApplet();

    void Close() override;
    void ReconfigureControllers(void * user_data, ControllerReconfigureFn on_complete, const ControllerHostParameters * parameters) override;

private:
    Core::HID::HIDCore & hid_core;
};
