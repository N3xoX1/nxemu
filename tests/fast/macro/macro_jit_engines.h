// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include "yuzu_video_core/macro/macro.h"

// Each production engine is compiled in its own translation unit.
std::unique_ptr<Tegra::CachedMacro> MakeTestInterpreter(Tegra::Engines::Maxwell3D& maxwell3d,
                                                        const std::vector<u32>& code);
std::unique_ptr<Tegra::CachedMacro> MakeTestJit(Tegra::Engines::Maxwell3D& maxwell3d,
                                                const std::vector<u32>& code);
