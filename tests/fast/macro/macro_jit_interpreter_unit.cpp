// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/macro/macro_interpreter.cpp"
#include "macro_jit_engines.h"

namespace Tegra {
// Satisfy the base engine's constructor, destructor and RTTI without pulling in the GPU renderer.
class HLEMacro {};
MacroEngine::MacroEngine(Engines::Maxwell3D& engine) : maxwell3d{engine} {}
MacroEngine::~MacroEngine() = default;
} // namespace Tegra

std::unique_ptr<Tegra::CachedMacro> MakeTestInterpreter(Tegra::Engines::Maxwell3D& maxwell3d,
                                                        const std::vector<u32>& code) {
    return std::make_unique<Tegra::MacroInterpreterImpl>(maxwell3d, code);
}
