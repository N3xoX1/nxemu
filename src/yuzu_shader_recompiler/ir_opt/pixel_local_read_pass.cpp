// SPDX-FileCopyrightText: Copyright 2026 nxemu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <vector>

#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/ir/modifiers.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_shader_recompiler/shader_info.h"

namespace Shader::Optimization {
namespace {
using IR::Opcode;

const IR::Inst* InstOf(const IR::Value& value, Opcode opcode) {
    const IR::Value resolved{value.Resolve()};
    if (resolved.IsImmediate()) {
        return nullptr;
    }
    const IR::Inst* const inst{resolved.InstRecursive()};
    return inst->GetOpcode() == opcode ? inst : nullptr;
}

bool Same(const IR::Value& lhs, const IR::Value& rhs) {
    return lhs.Resolve() == rhs.Resolve();
}

bool IsHalf(const IR::Value& value) {
    const IR::Value resolved{value.Resolve()};
    return resolved.IsImmediate() && resolved.Type() == IR::Type::F32 && resolved.F32() == 0.5f;
}

bool IsGet(const IR::Value& value, IR::Attribute attribute) {
    const IR::Inst* const get{InstOf(value, Opcode::GetAttribute)};
    return get && get->Arg(0).Attribute() == attribute;
}

/// value == operand * 0.5
bool IsHalfOf(const IR::Value& value, auto&& operand) {
    const IR::Inst* const mul{InstOf(value, Opcode::FPMul32)};
    return mul && ((IsHalf(mul->Arg(1)) && operand(mul->Arg(0))) ||
                   (IsHalf(mul->Arg(0)) && operand(mul->Arg(1))));
}

/// value == lhs + rhs, in any order
bool IsSum(const IR::Value& value, auto&& lhs, auto&& rhs) {
    const IR::Inst* const add{InstOf(value, Opcode::FPAdd32)};
    return add && ((lhs(add->Arg(0)) && rhs(add->Arg(1))) || (rhs(add->Arg(0)) && lhs(add->Arg(1))));
}

/// value == numerator / denominator, written as a multiplication by the reciprocal
bool IsQuotient(const IR::Value& value, auto&& numerator, auto&& denominator) {
    const auto is_reciprocal{[&](const IR::Value& factor) {
        const IR::Inst* const recip{InstOf(factor, Opcode::FPRecip32)};
        return recip && denominator(recip->Arg(0));
    }};
    const IR::Inst* const mul{InstOf(value, Opcode::FPMul32)};
    return mul && ((numerator(mul->Arg(0)) && is_reciprocal(mul->Arg(1))) ||
                   (is_reciprocal(mul->Arg(0)) && numerator(mul->Arg(1))));
}

/// Blocks run exactly once by every invocation that reaches the end of the program
std::vector<const IR::Block*> UnconditionalBlocks(const IR::Program& program) {
    std::vector<const IR::Block*> blocks;
    int depth{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Block:
            if (depth == 0) {
                blocks.push_back(node.data.block);
            }
            break;
        case IR::AbstractSyntaxNode::Type::If:
        case IR::AbstractSyntaxNode::Type::Loop:
            ++depth;
            break;
        case IR::AbstractSyntaxNode::Type::EndIf:
        case IR::AbstractSyntaxNode::Type::Repeat:
            --depth;
            break;
        case IR::AbstractSyntaxNode::Type::Return:
        case IR::AbstractSyntaxNode::Type::Unreachable:
            // An early exit leaves the outputs of some vertices at their defaults
            return depth == 0 ? blocks : std::vector<const IR::Block*>{};
        case IR::AbstractSyntaxNode::Type::Break:
            break;
        }
    }
    return blocks;
}

/// Screen coordinate of the fragment's own pixel, as (Gk.xy / Gk.w)
bool IsScreenPositionCoordinate(const IR::Program& program, const IR::Value& coords,
                                u32 screen_position_generics) {
    const IR::Inst* const vector{InstOf(coords, Opcode::CompositeConstructF32x2)};
    if (!vector || screen_position_generics == 0) {
        return false;
    }
    for (u32 index = 0; index < 32; ++index) {
        if (((screen_position_generics >> index) & 1) == 0 ||
            program.info.interpolation[index] != Interpolation::Smooth) {
            continue;
        }
        const IR::Attribute base{IR::Attribute::Generic0X + index * 4};
        const auto get{[](IR::Attribute attribute) {
            return [attribute](const IR::Value& value) { return IsGet(value, attribute); };
        }};
        if (IsQuotient(vector->Arg(0), get(base), get(base + 3)) &&
            IsQuotient(vector->Arg(1), get(base + 1), get(base + 3))) {
            return true;
        }
    }
    return false;
}

/// Integer coordinate of the fragment's own pixel, as ivec2(gl_FragCoord.xy)
bool IsFragCoordTexel(const IR::Value& coords) {
    const IR::Inst* const vector{InstOf(coords, Opcode::CompositeConstructU32x2)};
    if (!vector) {
        return false;
    }
    const auto is_texel{[](const IR::Value& value, IR::Attribute attribute) {
        const IR::Inst* convert{InstOf(value, Opcode::ConvertS32F32)};
        if (!convert) {
            convert = InstOf(value, Opcode::ConvertU32F32);
        }
        if (!convert) {
            return false;
        }
        // Pixel centers are at .5, so truncating and flooring both give the pixel
        IR::Value coord{convert->Arg(0)};
        if (const IR::Inst* const round{InstOf(coord, Opcode::FPFloor32)}) {
            coord = round->Arg(0);
        } else if (const IR::Inst* const trunc{InstOf(coord, Opcode::FPTrunc32)}) {
            coord = trunc->Arg(0);
        }
        return IsGet(coord, attribute);
    }};
    return is_texel(vector->Arg(0), IR::Attribute::PositionX) &&
           is_texel(vector->Arg(1), IR::Attribute::PositionY);
}

bool IsZeroOrEmpty(const IR::Value& value) {
    if (value.IsEmpty()) {
        return true;
    }
    const IR::Value resolved{value.Resolve()};
    return resolved.IsImmediate() && resolved.Type() == IR::Type::U32 && resolved.U32() == 0;
}

PixelLocalRead ClassifyRead(const IR::Program& program, IR::Inst& inst,
                            u32 screen_position_generics) {
    if (inst.GetAssociatedPseudoOperation(Opcode::GetSparseFromOp)) {
        return PixelLocalRead::None;
    }
    switch (inst.GetOpcode()) {
    case Opcode::ImageSampleImplicitLod:
    case Opcode::ImageSampleExplicitLod:
    case Opcode::ImageGradient:
        // A single level view makes the level of detail irrelevant
        if (!inst.Arg(3).IsEmpty() ||
            !IsScreenPositionCoordinate(program, inst.Arg(1), screen_position_generics)) {
            return PixelLocalRead::None;
        }
        return PixelLocalRead::ScreenPosition;
    case Opcode::ImageFetch:
        if (!inst.Arg(2).IsEmpty() || !IsZeroOrEmpty(inst.Arg(3)) || !inst.Arg(4).IsEmpty() ||
            !IsFragCoordTexel(inst.Arg(1))) {
            return PixelLocalRead::None;
        }
        return PixelLocalRead::FragCoord;
    default:
        return PixelLocalRead::None;
    }
}

bool IsTextureInst(Opcode opcode) {
    switch (opcode) {
    case Opcode::ImageSampleImplicitLod:
    case Opcode::ImageSampleExplicitLod:
    case Opcode::ImageSampleDrefImplicitLod:
    case Opcode::ImageSampleDrefExplicitLod:
    case Opcode::ImageGather:
    case Opcode::ImageGatherDref:
    case Opcode::ImageFetch:
    case Opcode::ImageQueryDimensions:
    case Opcode::ImageQueryLod:
    case Opcode::ImageGradient:
        return true;
    default:
        return false;
    }
}
} // Anonymous namespace

u32 FindScreenPositionGenerics(const IR::Program& program) {
    if (program.stage != Stage::VertexB && program.stage != Stage::TessellationEval) {
        return 0;
    }
    const std::vector<const IR::Block*> unconditional{UnconditionalBlocks(program)};
    constexpr size_t NUM_OUTPUTS{(IR::NUM_GENERICS + 1) * 4};
    std::array<IR::Value, NUM_OUTPUTS> values{};
    std::array<u32, NUM_OUTPUTS> num_stores{};
    std::array<bool, NUM_OUTPUTS> is_unconditional{};
    const auto output_index{[](IR::Attribute attribute) -> size_t {
        if (attribute >= IR::Attribute::PositionX && attribute <= IR::Attribute::PositionW) {
            return static_cast<size_t>(attribute) - static_cast<size_t>(IR::Attribute::PositionX);
        }
        if (IR::IsGeneric(attribute)) {
            return 4 + static_cast<size_t>(attribute) -
                   static_cast<size_t>(IR::Attribute::Generic0X);
        }
        return NUM_OUTPUTS;
    }};
    for (const IR::Block* const block : program.blocks) {
        const bool block_is_unconditional{std::ranges::find(unconditional, block) !=
                                          unconditional.end()};
        for (const IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case Opcode::SetAttributeIndexed:
                return 0;
            case Opcode::SetAttribute: {
                const IR::Attribute attribute{inst.Arg(0).Attribute()};
                if (attribute == IR::Attribute::ViewportIndex) {
                    // Only the first viewport is checked against the sampled texture
                    return 0;
                }
                const size_t index{output_index(attribute)};
                if (index < NUM_OUTPUTS) {
                    values[index] = inst.Arg(1);
                    ++num_stores[index];
                    is_unconditional[index] = block_is_unconditional;
                }
                break;
            }
            default:
                break;
            }
        }
    }
    const auto is_stored{[&](size_t index) {
        return num_stores[index] == 1 && is_unconditional[index];
    }};
    if (!is_stored(0) || !is_stored(1) || !is_stored(3)) {
        return 0;
    }
    const IR::Value& px{values[0]};
    const IR::Value& py{values[1]};
    const IR::Value& pw{values[3]};
    const auto same{[](const IR::Value& expected) {
        return [&expected](const IR::Value& value) { return Same(value, expected); };
    }};
    const auto negated{[](const IR::Value& expected) {
        return [&expected](const IR::Value& value) {
            const IR::Inst* const neg{InstOf(value, Opcode::FPNeg32)};
            return neg && Same(neg->Arg(0), expected);
        };
    }};
    u32 generics{};
    for (u32 generic = 0; generic < IR::NUM_GENERICS; ++generic) {
        const size_t base{4 + generic * 4};
        if (!is_stored(base) || !is_stored(base + 1) || !is_stored(base + 3)) {
            continue;
        }
        // (x + w) / 2 and (w - y) / 2 make xy / w the viewport coordinate of the fragment
        const bool x{IsHalfOf(values[base], [&](const IR::Value& value) {
            return IsSum(value, same(px), same(pw));
        })};
        const bool y{IsHalfOf(values[base + 1], [&](const IR::Value& value) {
            return IsSum(value, negated(py), same(pw));
        })};
        if (x && y && Same(values[base + 3], pw)) {
            generics |= 1U << generic;
        }
    }
    return generics;
}

bool FindPixelLocalTextureReads(IR::Program& program, u32 screen_position_generics) {
    if (program.stage != Stage::Fragment) {
        return false;
    }
    auto& descriptors{program.info.texture_descriptors};
    std::vector<PixelLocalRead> reads(descriptors.size(), PixelLocalRead::None);
    std::vector<bool> rejected(descriptors.size(), false);
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsTextureInst(inst.GetOpcode())) {
                continue;
            }
            const auto info{inst.Flags<IR::TextureInstInfo>()};
            if (info.type == TextureType::Buffer) {
                continue;
            }
            const size_t index{info.descriptor_index};
            const PixelLocalRead read{ClassifyRead(program, inst, screen_position_generics)};
            if (read == PixelLocalRead::None) {
                rejected[index] = true;
            } else if (reads[index] != PixelLocalRead::ScreenPosition) {
                // A screen position read also needs the viewport to match the texture
                reads[index] = read;
            }
        }
    }
    bool found{};
    u32 num_reads{};
    for (size_t index = 0; index < descriptors.size(); ++index) {
        TextureDescriptor& desc{descriptors[index]};
        desc.pixel_local_read = PixelLocalRead::None;
        if (rejected[index] || reads[index] == PixelLocalRead::None || desc.count != 1 ||
            desc.is_direct || desc.in_table_set || !desc.sources.empty() ||
            desc.type != TextureType::Color2D || desc.is_depth || desc.is_multisample ||
            num_reads == MAX_PIXEL_LOCAL_READS) {
            continue;
        }
        desc.pixel_local_read = reads[index];
        ++num_reads;
        found = true;
    }
    return found;
}

} // namespace Shader::Optimization
