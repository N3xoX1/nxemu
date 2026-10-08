// SPDX-FileCopyrightText: Copyright 2026 NxEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <optional>

#include "yuzu_shader_recompiler/frontend/ir/attribute.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/frontend/ir/value.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"

namespace Shader::Optimization {
namespace {
constexpr size_t MAX_VERTICES = 3;

bool HasSideEffects(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::SetAttributeIndexed:
    case IR::Opcode::SetPatch:
    case IR::Opcode::WriteGlobalU8:
    case IR::Opcode::WriteGlobalS8:
    case IR::Opcode::WriteGlobalU16:
    case IR::Opcode::WriteGlobalS16:
    case IR::Opcode::WriteGlobal32:
    case IR::Opcode::WriteGlobal64:
    case IR::Opcode::WriteGlobal128:
    case IR::Opcode::WriteStorageU8:
    case IR::Opcode::WriteStorageS8:
    case IR::Opcode::WriteStorageU16:
    case IR::Opcode::WriteStorageS16:
    case IR::Opcode::WriteStorage32:
    case IR::Opcode::WriteStorage64:
    case IR::Opcode::WriteStorage128:
    case IR::Opcode::ImageWrite:
    case IR::Opcode::BindlessImageWrite:
    case IR::Opcode::BoundImageWrite:
        return true;
    default:
        return false;
    }
}
} // Anonymous namespace

std::optional<IR::Attribute> FindForwardedLayerAttribute(const IR::Program& program,
                                                         u32 input_vertices) {
    if (program.stage != Stage::Geometry || program.is_geometry_passthrough ||
        program.invocations > 1 || input_vertices != MAX_VERTICES ||
        program.output_vertices != input_vertices ||
        program.output_topology != OutputTopology::TriangleStrip) {
        return std::nullopt;
    }
    std::optional<IR::Attribute> layer_source;
    std::array<IR::Value, MAX_VERTICES> emitted_vertices;
    std::optional<IR::Value> current_vertex;
    size_t num_emitted{};
    for (const IR::Block* const block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            const IR::Opcode opcode{inst.GetOpcode()};
            if (HasSideEffects(opcode)) {
                return std::nullopt;
            }
            if (opcode == IR::Opcode::EmitVertex) {
                if (!current_vertex || num_emitted == MAX_VERTICES) {
                    return std::nullopt;
                }
                for (size_t index = 0; index < num_emitted; ++index) {
                    if (emitted_vertices[index] == *current_vertex) {
                        return std::nullopt;
                    }
                }
                emitted_vertices[num_emitted++] = *current_vertex;
                current_vertex.reset();
                continue;
            }
            if (opcode != IR::Opcode::SetAttribute) {
                continue;
            }
            // Every output has to be the same attribute of one input vertex
            const IR::Attribute target{inst.Arg(0).Attribute()};
            const IR::Value value{inst.Arg(1).Resolve()};
            if (value.IsImmediate()) {
                return std::nullopt;
            }
            const IR::Inst* const source{value.InstRecursive()};
            if (source->GetOpcode() != IR::Opcode::GetAttribute) {
                return std::nullopt;
            }
            const IR::Attribute source_attribute{source->Arg(0).Attribute()};
            const IR::Value vertex{source->Arg(1).Resolve()};
            if (current_vertex && !(*current_vertex == vertex)) {
                return std::nullopt;
            }
            current_vertex = vertex;
            if (target == IR::Attribute::Layer) {
                if (!IR::IsGeneric(source_attribute) ||
                    (layer_source && *layer_source != source_attribute)) {
                    return std::nullopt;
                }
                layer_source = source_attribute;
            } else if (target != source_attribute) {
                return std::nullopt;
            }
        }
    }
    if (num_emitted != input_vertices || current_vertex) {
        return std::nullopt;
    }
    return layer_source;
}

} // namespace Shader::Optimization
