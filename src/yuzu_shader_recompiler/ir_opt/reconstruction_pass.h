// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include <vector>

namespace Shader::Optimization
{
// Uniform UV-per-pixel coefficients inferred from the coordinate expressions.
// No game identity, shader hash, cbuf bank or cbuf offset is prescribed.
struct ReconstructionSteps
{
    std::vector<IR::Inst *> loads;
    std::vector<u32> textures;
};
[[nodiscard]] ReconstructionSteps FindReconstructionSteps(const IR::Program & program);
[[nodiscard]] IR::U1 ScaleReconstructionSteps(IR::Program & program, const ReconstructionSteps & steps);
} // namespace Shader::Optimization
