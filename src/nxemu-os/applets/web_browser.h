// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <nxemu-module-spec/operating_system.h>

class DefaultWebBrowserApplet final :
    public IWebBrowserApplet
{
public:
    void Close() override;

    void OpenLocalWebPage(const char * local_url_utf8, void * extract_user_data, ExtractRomFsFn extract_romfs, void * open_user_data, OpenWebPageFn open_callback) const override;
    void OpenExternalWebPage(const char * external_url_utf8, void * user_data, OpenWebPageFn callback) const override;
};