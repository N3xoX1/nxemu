// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>

#include "core/hle/service/am/applet.h"

namespace Core {
class System;
}

namespace Service::AM {

class EventObserver;
class WindowSystem;

enum class LaunchType {
    FrontendInitiated,
    ApplicationInitiated,
};

struct FrontendAppletParameters {
    ProgramId program_id{};
    AppletId applet_id{};
    AppletType applet_type{};
    LaunchType launch_type{};
    s32 program_index{};
    s32 previous_program_index{-1};
};

class AppletManager {
public:
    explicit AppletManager(Core::System& system);
    ~AppletManager();

    void InsertApplet(std::shared_ptr<Applet> applet);
    void NotifyAppletStarted(AppletResourceUserId aruid);
    bool EnsureHidRegistered(AppletResourceUserId aruid);

    void CreateAndInsertByFrontendAppletParameters(AppletResourceUserId aruid,
                                                   const FrontendAppletParameters& params,
                                                   Kernel::KProcess* lifecycle_process = nullptr);
    std::shared_ptr<Applet> GetByAppletResourceUserId(AppletResourceUserId aruid) const;
    std::shared_ptr<Applet> GetApplicationApplet() const;

    void StopEventObserver();
    void Reset();

    void RequestExit();
    void RequestResume();
    void OperationModeChanged();
    void FocusStateChanged();

    WindowSystem& GetWindowSystem();
    const WindowSystem& GetWindowSystem() const;

private:
    void EnsureEventObserver();

    Core::System& m_system;

    std::unique_ptr<WindowSystem> m_window_system;
    std::mutex m_event_observer_mutex;
    std::unique_ptr<EventObserver> m_event_observer;

    // AudioController state goes here
};

} // namespace Service::AM
