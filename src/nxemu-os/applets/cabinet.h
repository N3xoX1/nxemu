// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <nxemu-module-spec/operating_system.h>

class DefaultCabinetApplet final : 
    public ICabinetApplet
{
public:
    void Close() override;
    void ShowCabinetApplet(void * user_data, CabinetFinishedFn on_finished, const CabinetParameters * parameters) override;
};