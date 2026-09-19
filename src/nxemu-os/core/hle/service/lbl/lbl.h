// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core {
class System;
}

namespace Service::LBL {

struct AmbientLightSensorState {
    bool over_limit{};
    float lux{};
};

// AM 66/67/71 forward ambient-light queries to lbl.
AmbientLightSensorState GetAmbientLightSensorState(Core::System& system);
bool IsAmbientLightSensorAvailable(Core::System& system);

void LoopProcess(Core::System& system);

} // namespace Service::LBL
