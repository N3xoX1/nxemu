// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <nxemu-module-spec/operating_system.h>

class DefaultMiiEditApplet final :
    public IMiiEditApplet
{
public:
    void Close() override;
    void ShowMiiEdit(void * user_data, SimpleFinishedFn finished) const override;
};