// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "yuzu_shader_recompiler/frontend/ir/attribute.h"
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/vtg_as_compute.h"

namespace Shader {
struct HostTranslateInfo;
}

namespace Shader::Optimization {

void CollectShaderInfoPass(Environment& env, IR::Program& program);
void ConditionalBarrierPass(IR::Program& program);
void ConstantPropagationPass(Environment& env, IR::Program& program);
void DeadCodeEliminationPass(IR::Program& program);
void GlobalMemoryToStorageBufferPass(IR::Program& program, const HostTranslateInfo& host_info);
void IdentityRemovalPass(IR::Program& program);
void LowerFp64ToFp32(IR::Program& program);
void LowerFp16ToFp32(IR::Program& program);
void LowerInt64ToInt32(IR::Program& program);
void RescalingPass(IR::Program& program);
void SsaRewritePass(IR::Program& program);
void PositionPass(Environment& env, IR::Program& program);
void TexturePass(Environment& env, IR::Program& program, const HostTranslateInfo& host_info);
// SPIR-V only: compact oversized compute shared memory when every live access fits XYZ.
// Preserves valid guest accesses; does not prove or repair guest out-of-bounds addresses.
// On failure the program is unchanged. Run after optimization and before SPIR-V emission.
[[nodiscard]] bool TryPackSharedMemory16To12(IR::Program& program, u32 max_shared_memory);
// Returns the input attribute a geometry shader copies into the layer when all it does is
// forward its input triangle unchanged and select the layer, so the previous stage can do it.
[[nodiscard]] std::optional<IR::Attribute> FindForwardedLayerAttribute(const IR::Program& program,
                                                                       u32 input_vertices);
// Turns a vertex shader into a compute shader pulling its attributes from the vertex buffers and
// writing its outputs to a storage buffer. On failure the program is unchanged.
[[nodiscard]] bool VtgVertexToCompute(IR::Program& program, const VtgVertexInputs& inputs,
                                      const VtgVaryingLayout& layout, VtgVertexBindings& bindings);
// Turns a geometry shader into a compute shader reading the outputs of the vertex stage and
// writing its vertices and a strip index buffer. On failure the program is unchanged.
[[nodiscard]] bool VtgGeometryToCompute(IR::Program& program, VtgTopology topology,
                                        u32 input_vertices, const VtgVaryingLayout& input_layout,
                                        const VtgVaryingLayout& output_layout,
                                        VtgGeometryBindings& bindings);
void LayerPass(IR::Program& program, const HostTranslateInfo& host_info);
void VendorWorkaroundPass(IR::Program& program);
void VerificationPass(const IR::Program& program);

// Dual Vertex
void VertexATransformPass(IR::Program& program);
void VertexBTransformPass(IR::Program& program);
void JoinTextureInfo(Info& base, Info& source);
void JoinStorageInfo(Info& base, Info& source);

} // namespace Shader::Optimization
