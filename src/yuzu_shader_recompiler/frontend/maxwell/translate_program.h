// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/vtg_as_compute.h"
#include "yuzu_shader_recompiler/frontend/maxwell/control_flow.h"
#include "yuzu_shader_recompiler/object_pool.h"
#include "yuzu_shader_recompiler/runtime_info.h"

namespace Shader {
struct HostTranslateInfo;
}

namespace Shader::Maxwell {

[[nodiscard]] IR::Program TranslateProgram(ObjectPool<IR::Inst>& inst_pool,
                                           ObjectPool<IR::Block>& block_pool, Environment& env,
                                           Flow::CFG& cfg, const HostTranslateInfo& host_info);

[[nodiscard]] IR::Program MergeDualVertexPrograms(IR::Program& vertex_a, IR::Program& vertex_b,
                                                  Environment& env_vertex_b);

void ConvertLegacyToGeneric(IR::Program& program, const RuntimeInfo& runtime_info);

// Maxwell v1 and older Nvidia cards don't support setting gl_Layer from non-geometry stages.
// This creates a workaround by setting the layer as a generic output and creating a
// passthrough geometry shader that reads the generic and sets the layer.
[[nodiscard]] IR::Program GenerateGeometryPassthrough(ObjectPool<IR::Inst>& inst_pool,
                                                      ObjectPool<IR::Block>& block_pool,
                                                      const HostTranslateInfo& host_info,
                                                      IR::Program& source_program,
                                                      Shader::OutputTopology output_topology);

// Creates the vertex shader feeding the rasterizer with the vertices written to a storage buffer
// by the stages running as compute, indexed by the vertex index of the draw.
[[nodiscard]] IR::Program GenerateVtgPassthroughVertex(ObjectPool<IR::Inst>& inst_pool,
                                                       ObjectPool<IR::Block>& block_pool,
                                                       const IR::Program& source_program,
                                                       const VaryingState& source_stores,
                                                       const VtgVaryingLayout& layout);

} // namespace Shader::Maxwell
