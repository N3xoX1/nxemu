// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/service/am/frontend/applets.h"

namespace Core {
class System;
}

namespace Service::AM::Frontend {

enum class AuthAppletType : u32 {
    ShowParentalAuthentication,
    RegisterParentalPasscode,
    ChangeParentalPasscode,
};

class Auth final : public FrontendApplet {
public:
    explicit Auth(Core::System& system_, std::shared_ptr<Applet> applet_,
                  LibraryAppletMode applet_mode_);
    ~Auth() override;

    void Initialize() override;
    Result GetStatus() const override;
    void ExecuteInteractive() override;
    void Execute() override;
    Result RequestExit() override;

    void AuthFinished(bool is_successful = true);

private:
    bool complete = false;
    bool successful = false;

    AuthAppletType type = AuthAppletType::ShowParentalAuthentication;
    u8 arg0 = 0;
    u8 arg1 = 0;
    u8 arg2 = 0;
};

enum class PhotoViewerAppletMode : u8 {
    CurrentApp = 0,
    AllApps = 1,
};

class PhotoViewer final : public FrontendApplet {
public:
    explicit PhotoViewer(Core::System& system_, std::shared_ptr<Applet> applet_,
                         LibraryAppletMode applet_mode_);
    ~PhotoViewer() override;

    void Initialize() override;
    Result GetStatus() const override;
    void ExecuteInteractive() override;
    void Execute() override;
    Result RequestExit() override;

    void ViewFinished();

private:
    bool complete = false;
    PhotoViewerAppletMode mode = PhotoViewerAppletMode::CurrentApp;
};

// Based on Eden's NetConnect frontend; headless completion reports an offline result.
class NetConnect final : public FrontendApplet {
public:
    using FrontendApplet::FrontendApplet;
    Result GetStatus() const override;
    void ExecuteInteractive() override;
    void Execute() override;
    Result RequestExit() override;
private:
    bool complete{};
};

class StubApplet final : public FrontendApplet {
public:
    explicit StubApplet(Core::System& system_, std::shared_ptr<Applet> applet_, AppletId id_,
                        LibraryAppletMode applet_mode_);
    ~StubApplet() override;

    void Initialize() override;

    Result GetStatus() const override;
    void ExecuteInteractive() override;
    void Execute() override;
    Result RequestExit() override;

private:
    AppletId id;
};

} // namespace Service::AM::Frontend
