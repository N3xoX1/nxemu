// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/macro/macro_jit_x64.cpp"
#include "macro_jit_engines.h"

std::unique_ptr<Tegra::CachedMacro> MakeTestJit(Tegra::Engines::Maxwell3D& maxwell3d,
                                                const std::vector<u32>& code) {
    return std::make_unique<Tegra::MacroJITx64Impl>(maxwell3d, code);
}
