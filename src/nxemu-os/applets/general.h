// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "applets/applet.h"
#include <nxemu-module-spec/operating_system.h>

class DefaultParentalControlsApplet final : 
    public IParentalControlsApplet
{
public:
    void Close() override;
    void VerifyPIN(void * user_data, BoolFinishedFn finished, bool suspend_future_verification_temporarily) override;
    void VerifyPINForSettings(void * user_data, BoolFinishedFn finished) override;
    void RegisterPIN(void * user_data, SimpleFinishedFn finished) override;
    void ChangePIN(void * user_data, SimpleFinishedFn finished) override;
};

class DefaultPhotoViewerApplet final : 
    public IPhotoViewerApplet
{
public:
    void Close() override;
    void ShowPhotosForApplication(uint64_t title_id, void * user_data, SimpleFinishedFn finished) const override;
    void ShowAllPhotos(void * user_data, SimpleFinishedFn finished) const override;
};