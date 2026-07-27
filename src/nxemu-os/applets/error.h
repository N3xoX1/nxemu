// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>

#include "applets/applet.h"
#include "core/hle/result.h"
#include <nxemu-module-spec/operating_system.h>

class ErrorApplet : public Applet
{
public:
    using FinishedCallback = std::function<void()>;

    virtual ~ErrorApplet();
    virtual void ShowError(Result error, FinishedCallback finished) const = 0;
    virtual void ShowErrorWithTimestamp(Result error, std::chrono::seconds time, FinishedCallback finished) const = 0;
    virtual void ShowCustomErrorText(Result error, std::string dialog_text, std::string fullscreen_text, FinishedCallback finished) const = 0;
};

class DefaultErrorApplet final :
    public IErrorApplet
{
public:
    void Close() override;
    void ShowError(uint32_t result_raw, void * user_data, SimpleFinishedFn finished) const override;
    void ShowErrorWithTimestamp(uint32_t result_raw, int64_t time_unix_seconds, void * user_data, SimpleFinishedFn finished) const override;
    void ShowCustomErrorText(uint32_t result_raw, const char * dialog_text_utf8, const char * fullscreen_text_utf8, void * user_data, SimpleFinishedFn finished) const override;
};
