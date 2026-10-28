// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/host1x/syncpoint_manager.h"
namespace Tegra::Host1x {
class Host1x {
public:
    SyncpointManager& GetSyncpointManager() { return manager; }
private:
    SyncpointManager manager;
};
}
