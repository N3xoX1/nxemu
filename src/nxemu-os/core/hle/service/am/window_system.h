// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "core/hle/service/am/am_types.h"

namespace Core {
class System;
}

namespace Service::AM {

struct Applet;
class AppletDataBroker;
class EventObserver;

class WindowSystem {
public:
    explicit WindowSystem(Core::System& system);
    ~WindowSystem();

    void SetEventObserver(EventObserver* event_observer);

    void TrackApplet(std::shared_ptr<Applet> applet, bool is_application);
    void Reset();

    std::shared_ptr<Applet> GetByAppletResourceUserId(AppletResourceUserId aruid) const;
    std::shared_ptr<Applet> GetApplicationApplet() const;

    void RequestApplicationToGetForeground();
    void RequestHomeMenuToGetForeground();
    void RequestAppletVisibilityState(Applet& applet, bool visible);
    void NotifyAppletStarted(Applet& applet);
    void NotifyAppletStopped(Applet& applet);
    void NotifyAppletStateChanged();

    void OnExitRequested();
    void OnResumeRequested();
    void OnOperationModeChanged();
    void Update();

private:
    bool PruneTerminatedAppletsLocked(
        std::vector<std::shared_ptr<AppletDataBroker>>& completed_brokers,
        std::vector<std::shared_ptr<Applet>>& children_to_terminate);
    void UpdateAppletStateLocked(Applet* applet, bool is_foreground, bool overlay_takes_input);

    Core::System& m_system;
    EventObserver* m_event_observer{};
    mutable std::mutex m_lock{};
    std::map<AppletResourceUserId, std::shared_ptr<Applet>> m_applets{};
    Applet* m_home_menu{};
    Applet* m_application{};
    Applet* m_overlay_display{};
    Applet* m_foreground_requested_applet{};
};

} // namespace Service::AM
