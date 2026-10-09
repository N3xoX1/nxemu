// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <boost/container/small_vector.hpp>

#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/ir/breadth_first_search.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_shader_recompiler/ir_opt/bindless_descriptor.h"
#include "yuzu_shader_recompiler/shader_info.h"

namespace Shader::Optimization {
namespace {
constexpr u32 DESCRIPTOR_SIZE = 8;
constexpr u32 DESCRIPTOR_SIZE_SHIFT = static_cast<u32>(std::countr_zero(DESCRIPTOR_SIZE));

struct ConstBufferAddr {
    u32 index;
    u32 offset;
    u32 shift_left;
    u32 secondary_index;
    u32 secondary_offset;
    u32 secondary_shift_left;
    IR::U32 dynamic_offset;
    u32 count;
    u32 size_shift{DESCRIPTOR_SIZE_SHIFT};
    bool has_secondary;
};

struct TextureInst {
    ConstBufferAddr cbuf;
    IR::Inst* inst;
    IR::Block* block;
};

using TextureInstVector = boost::container::small_vector<TextureInst, 24>;

// Bindless handles may be fields of material records rather than consecutive handles.
// Track alignment through SSA merges so indexing and descriptor uploads use the same stride.
u32 OffsetAlignment(const IR::Value& value, std::unordered_set<const IR::Inst*>& visiting,
                    std::unordered_map<const IR::Inst*, u32>& alignments, u32 depth = 0) {
    if (value.IsImmediate()) {
        return static_cast<u32>(std::countr_zero(value.U32()));
    }
    const IR::Inst* inst{value.InstRecursive()};
    if (depth >= 128 || !inst) {
        return 0;
    }
    if (const auto cached = alignments.find(inst); cached != alignments.end()) {
        return cached->second;
    }
    if (!visiting.insert(inst).second) {
        return 0;
    }
    const auto alignment = [&] {
        switch (inst->GetOpcode()) {
        case IR::Opcode::UndefU32:
            // Undefined predecessors impose no constraint on the defined value.
            return 32U;
        case IR::Opcode::ShiftLeftLogical32:
            if (inst->Arg(1).IsImmediate() && inst->Arg(1).U32() < 32) {
                return std::min(32U, inst->Arg(1).U32() +
                    OffsetAlignment(inst->Arg(0), visiting, alignments, depth + 1));
            }
            return 0U;
        case IR::Opcode::Phi: {
            u32 result{32};
            for (size_t i = 0; i < inst->NumArgs(); ++i) {
                result = std::min(result, OffsetAlignment(inst->Arg(i), visiting, alignments, depth + 1));
            }
            return result;
        }
        case IR::Opcode::BitwiseAnd32:
            // AND guarantees every zero low bit guaranteed by either operand.
            return std::max(OffsetAlignment(inst->Arg(0), visiting, alignments, depth + 1),
                            OffsetAlignment(inst->Arg(1), visiting, alignments, depth + 1));
        case IR::Opcode::IAdd32:
        case IR::Opcode::ISub32:
        case IR::Opcode::BitwiseOr32:
        case IR::Opcode::BitwiseXor32:
            // Addition/subtraction wrap in U32, preserving the common power-of-two divisor.
            return std::min(OffsetAlignment(inst->Arg(0), visiting, alignments, depth + 1),
                            OffsetAlignment(inst->Arg(1), visiting, alignments, depth + 1));
        case IR::Opcode::SelectU32:
            return std::min(OffsetAlignment(inst->Arg(1), visiting, alignments, depth + 1),
                            OffsetAlignment(inst->Arg(2), visiting, alignments, depth + 1));
        default:
            return 0U;
        }
    }();
    visiting.erase(inst);
    // SSA is a graph: repeated phi/select predecessors must not multiply traversal work.
    alignments.emplace(inst, alignment);
    return alignment;
}

u32 DynamicDescriptorSizeShift(const ConstBufferAddr& addr,
                               std::unordered_set<const IR::Inst*>& visiting,
                               std::unordered_map<const IR::Inst*, u32>& alignments) {
    if (addr.count <= 1) {
        return DESCRIPTOR_SIZE_SHIFT;
    }
    const u32 alignment{OffsetAlignment(addr.dynamic_offset, visiting, alignments)};
    // Alignment is a guaranteed divisor of every possible byte offset, not a minimum
    // eight-byte record size. A U32 handle can occupy a four-byte field (or be unaligned).
    return std::min(alignment, 31U);
}

IR::Opcode IndexedInstruction(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::BindlessImageSampleImplicitLod:
    case IR::Opcode::BoundImageSampleImplicitLod:
        return IR::Opcode::ImageSampleImplicitLod;
    case IR::Opcode::BoundImageSampleExplicitLod:
    case IR::Opcode::BindlessImageSampleExplicitLod:
        return IR::Opcode::ImageSampleExplicitLod;
    case IR::Opcode::BoundImageSampleDrefImplicitLod:
    case IR::Opcode::BindlessImageSampleDrefImplicitLod:
        return IR::Opcode::ImageSampleDrefImplicitLod;
    case IR::Opcode::BoundImageSampleDrefExplicitLod:
    case IR::Opcode::BindlessImageSampleDrefExplicitLod:
        return IR::Opcode::ImageSampleDrefExplicitLod;
    case IR::Opcode::BindlessImageGather:
    case IR::Opcode::BoundImageGather:
        return IR::Opcode::ImageGather;
    case IR::Opcode::BindlessImageGatherDref:
    case IR::Opcode::BoundImageGatherDref:
        return IR::Opcode::ImageGatherDref;
    case IR::Opcode::BindlessImageFetch:
    case IR::Opcode::BoundImageFetch:
        return IR::Opcode::ImageFetch;
    case IR::Opcode::BoundImageQueryDimensions:
    case IR::Opcode::BindlessImageQueryDimensions:
        return IR::Opcode::ImageQueryDimensions;
    case IR::Opcode::BoundImageQueryLod:
    case IR::Opcode::BindlessImageQueryLod:
        return IR::Opcode::ImageQueryLod;
    case IR::Opcode::BoundImageGradient:
    case IR::Opcode::BindlessImageGradient:
        return IR::Opcode::ImageGradient;
    case IR::Opcode::BoundImageRead:
    case IR::Opcode::BindlessImageRead:
        return IR::Opcode::ImageRead;
    case IR::Opcode::BoundImageWrite:
    case IR::Opcode::BindlessImageWrite:
        return IR::Opcode::ImageWrite;
    case IR::Opcode::BoundImageAtomicIAdd32:
    case IR::Opcode::BindlessImageAtomicIAdd32:
        return IR::Opcode::ImageAtomicIAdd32;
    case IR::Opcode::BoundImageAtomicSMin32:
    case IR::Opcode::BindlessImageAtomicSMin32:
        return IR::Opcode::ImageAtomicSMin32;
    case IR::Opcode::BoundImageAtomicUMin32:
    case IR::Opcode::BindlessImageAtomicUMin32:
        return IR::Opcode::ImageAtomicUMin32;
    case IR::Opcode::BoundImageAtomicSMax32:
    case IR::Opcode::BindlessImageAtomicSMax32:
        return IR::Opcode::ImageAtomicSMax32;
    case IR::Opcode::BoundImageAtomicUMax32:
    case IR::Opcode::BindlessImageAtomicUMax32:
        return IR::Opcode::ImageAtomicUMax32;
    case IR::Opcode::BoundImageAtomicInc32:
    case IR::Opcode::BindlessImageAtomicInc32:
        return IR::Opcode::ImageAtomicInc32;
    case IR::Opcode::BoundImageAtomicDec32:
    case IR::Opcode::BindlessImageAtomicDec32:
        return IR::Opcode::ImageAtomicDec32;
    case IR::Opcode::BoundImageAtomicAnd32:
    case IR::Opcode::BindlessImageAtomicAnd32:
        return IR::Opcode::ImageAtomicAnd32;
    case IR::Opcode::BoundImageAtomicOr32:
    case IR::Opcode::BindlessImageAtomicOr32:
        return IR::Opcode::ImageAtomicOr32;
    case IR::Opcode::BoundImageAtomicXor32:
    case IR::Opcode::BindlessImageAtomicXor32:
        return IR::Opcode::ImageAtomicXor32;
    case IR::Opcode::BoundImageAtomicExchange32:
    case IR::Opcode::BindlessImageAtomicExchange32:
        return IR::Opcode::ImageAtomicExchange32;
    default:
        return IR::Opcode::Void;
    }
}

bool IsBindless(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::BindlessImageSampleImplicitLod:
    case IR::Opcode::BindlessImageSampleExplicitLod:
    case IR::Opcode::BindlessImageSampleDrefImplicitLod:
    case IR::Opcode::BindlessImageSampleDrefExplicitLod:
    case IR::Opcode::BindlessImageGather:
    case IR::Opcode::BindlessImageGatherDref:
    case IR::Opcode::BindlessImageFetch:
    case IR::Opcode::BindlessImageQueryDimensions:
    case IR::Opcode::BindlessImageQueryLod:
    case IR::Opcode::BindlessImageGradient:
    case IR::Opcode::BindlessImageRead:
    case IR::Opcode::BindlessImageWrite:
    case IR::Opcode::BindlessImageAtomicIAdd32:
    case IR::Opcode::BindlessImageAtomicSMin32:
    case IR::Opcode::BindlessImageAtomicUMin32:
    case IR::Opcode::BindlessImageAtomicSMax32:
    case IR::Opcode::BindlessImageAtomicUMax32:
    case IR::Opcode::BindlessImageAtomicInc32:
    case IR::Opcode::BindlessImageAtomicDec32:
    case IR::Opcode::BindlessImageAtomicAnd32:
    case IR::Opcode::BindlessImageAtomicOr32:
    case IR::Opcode::BindlessImageAtomicXor32:
    case IR::Opcode::BindlessImageAtomicExchange32:
        return true;
    case IR::Opcode::BoundImageSampleImplicitLod:
    case IR::Opcode::BoundImageSampleExplicitLod:
    case IR::Opcode::BoundImageSampleDrefImplicitLod:
    case IR::Opcode::BoundImageSampleDrefExplicitLod:
    case IR::Opcode::BoundImageGather:
    case IR::Opcode::BoundImageGatherDref:
    case IR::Opcode::BoundImageFetch:
    case IR::Opcode::BoundImageQueryDimensions:
    case IR::Opcode::BoundImageQueryLod:
    case IR::Opcode::BoundImageGradient:
    case IR::Opcode::BoundImageRead:
    case IR::Opcode::BoundImageWrite:
    case IR::Opcode::BoundImageAtomicIAdd32:
    case IR::Opcode::BoundImageAtomicSMin32:
    case IR::Opcode::BoundImageAtomicUMin32:
    case IR::Opcode::BoundImageAtomicSMax32:
    case IR::Opcode::BoundImageAtomicUMax32:
    case IR::Opcode::BoundImageAtomicInc32:
    case IR::Opcode::BoundImageAtomicDec32:
    case IR::Opcode::BoundImageAtomicAnd32:
    case IR::Opcode::BoundImageAtomicOr32:
    case IR::Opcode::BoundImageAtomicXor32:
    case IR::Opcode::BoundImageAtomicExchange32:
        return false;
    default:
        throw InvalidArgument("Invalid opcode {}", inst.GetOpcode());
    }
}

bool IsTextureInstruction(const IR::Inst& inst) {
    return IndexedInstruction(inst) != IR::Opcode::Void;
}

std::optional<ConstBufferAddr> TryGetConstBuffer(const IR::Inst* inst, Environment& env);

std::optional<ConstBufferAddr> Track(const IR::Value& value, Environment& env) {
    return IR::BreadthFirstSearch(
        value, [&env](const IR::Inst* inst) { return TryGetConstBuffer(inst, env); });
}

std::optional<ConstBufferAddr> TrackLocalLoad(const IR::Value& value, const IR::Block* block,
                                           Environment& env) {
    if (value.IsImmediate()) {
        return std::nullopt;
    }

    const IR::Inst* const load{value.InstRecursive()};
    if (load->GetOpcode() != IR::Opcode::LoadLocal) {
        return std::nullopt;
    }

    const IR::Value load_offset{load->Arg(0)};
    if (!load_offset.IsImmediate()) {
        return std::nullopt;
    }

    const IR::Inst* matching_write{};
    bool found_load{};
    for (const IR::Inst& candidate : block->Instructions()) {
        if (&candidate == load) {
            found_load = true;
            break;
        }
        if (candidate.GetOpcode() != IR::Opcode::WriteLocal) {
            continue;
        }

        const IR::Value write_offset{candidate.Arg(0)};
        // A dynamic local-memory write may alias the immediate slot loaded below.
        // Without memory SSA, leave this handle unresolved instead of guessing.
        if (!write_offset.IsImmediate()) {
            return std::nullopt;
        }
        if (write_offset.U32() != load_offset.U32()) {
            continue;
        }

        // Keep this fallback limited to a single store before the load in this block.
        if (matching_write != nullptr) {
            return std::nullopt;
        }
        matching_write = &candidate;
    }

    if (!found_load || matching_write == nullptr) {
        return std::nullopt;
    }
    const IR::Value stored_value{matching_write->Arg(1)};
    if (stored_value.IsImmediate()) {
        return std::nullopt;
    }
    const IR::Inst* const source{stored_value.InstRecursive()};
    // The general breadth-first tracker may find a cbuf anywhere in an expression,
    // including a partial store, an address calculation or a modified handle. Only
    // forward a scalar cbuf load (possibly through identities) whose value is unchanged.
    if (source->GetOpcode() != IR::Opcode::GetCbufU32) {
        return std::nullopt;
    }
    return TryGetConstBuffer(source, env);
}

std::optional<u32> TryGetConstant(IR::Value& value, Environment& env) {
    const IR::Inst* inst = value.InstRecursive();
    if (inst->GetOpcode() != IR::Opcode::GetCbufU32) {
        return std::nullopt;
    }
    const IR::Value index{inst->Arg(0)};
    const IR::Value offset{inst->Arg(1)};
    if (!index.IsImmediate()) {
        return std::nullopt;
    }
    if (!offset.IsImmediate()) {
        return std::nullopt;
    }
    const auto index_number = index.U32();
    if (index_number != 1) {
        return std::nullopt;
    }
    const auto offset_number = offset.U32();
    return env.ReadCbufValue(index_number, offset_number);
}

std::optional<ConstBufferAddr> TryGetConstBuffer(const IR::Inst* inst, Environment& env) {
    switch (inst->GetOpcode()) {
    default:
        return std::nullopt;
    case IR::Opcode::BitwiseOr32: {
        std::optional lhs{Track(inst->Arg(0), env)};
        std::optional rhs{Track(inst->Arg(1), env)};
        if (!lhs || !rhs) {
            return std::nullopt;
        }
        if (lhs->has_secondary || rhs->has_secondary) {
            return std::nullopt;
        }
        if (lhs->count > 1 || rhs->count > 1) {
            return std::nullopt;
        }
        if (lhs->shift_left > 0 || lhs->index > rhs->index || lhs->offset > rhs->offset) {
            std::swap(lhs, rhs);
        }
        return ConstBufferAddr{
            .index = lhs->index,
            .offset = lhs->offset,
            .shift_left = lhs->shift_left,
            .secondary_index = rhs->index,
            .secondary_offset = rhs->offset,
            .secondary_shift_left = rhs->shift_left,
            .dynamic_offset = {},
            .count = 1,
            .has_secondary = true,
        };
    }
    case IR::Opcode::ShiftLeftLogical32: {
        const IR::Value shift{inst->Arg(1)};
        if (!shift.IsImmediate()) {
            return std::nullopt;
        }
        std::optional lhs{Track(inst->Arg(0), env)};
        if (lhs) {
            lhs->shift_left = shift.U32();
        }
        return lhs;
        break;
    }
    case IR::Opcode::BitwiseAnd32: {
        IR::Value op1{inst->Arg(0)};
        IR::Value op2{inst->Arg(1)};
        if (op1.IsImmediate()) {
            std::swap(op1, op2);
        }
        if (!op2.IsImmediate() && !op1.IsImmediate()) {
            do {
                auto try_index = TryGetConstant(op1, env);
                if (try_index) {
                    op1 = op2;
                    op2 = IR::Value{*try_index};
                    break;
                }
                auto try_index_2 = TryGetConstant(op2, env);
                if (try_index_2) {
                    op2 = IR::Value{*try_index_2};
                    break;
                }
                return std::nullopt;
            } while (false);
        }
        std::optional lhs{Track(op1, env)};
        if (lhs) {
            lhs->shift_left = static_cast<u32>(std::countr_zero(op2.U32()));
        }
        return lhs;
        break;
    }
    case IR::Opcode::GetCbufU32x2:
    case IR::Opcode::GetCbufU32:
        break;
    }
    const IR::Value index{inst->Arg(0)};
    const IR::Value offset{inst->Arg(1)};
    if (!index.IsImmediate()) {
        // Reading a bindless texture from variable indices is valid
        // but not supported here at the moment
        return std::nullopt;
    }
    if (offset.IsImmediate()) {
        return ConstBufferAddr{
            .index = index.U32(),
            .offset = offset.U32(),
            .shift_left = 0,
            .secondary_index = 0,
            .secondary_offset = 0,
            .secondary_shift_left = 0,
            .dynamic_offset = {},
            .count = 1,
            .has_secondary = false,
        };
    }
    IR::Inst* const offset_inst{offset.InstRecursive()};
    if (offset_inst->GetOpcode() != IR::Opcode::IAdd32) {
        return std::nullopt;
    }
    u32 base_offset{};
    IR::U32 dynamic_offset;
    if (offset_inst->Arg(0).IsImmediate()) {
        base_offset = offset_inst->Arg(0).U32();
        dynamic_offset = IR::U32{offset_inst->Arg(1)};
    } else if (offset_inst->Arg(1).IsImmediate()) {
        base_offset = offset_inst->Arg(1).U32();
        dynamic_offset = IR::U32{offset_inst->Arg(0)};
    } else {
        return std::nullopt;
    }
    return ConstBufferAddr{
        .index = index.U32(),
        .offset = base_offset,
        .shift_left = 0,
        .secondary_index = 0,
        .secondary_offset = 0,
        .secondary_shift_left = 0,
        .dynamic_offset = dynamic_offset,
        .count = 8,
        .has_secondary = false,
    };
}

std::optional<ConstBufferAddr> TrackBindless(Environment& env, IR::Block* block, IR::Inst& inst) {
    std::optional<ConstBufferAddr> track_addr{Track(inst.Arg(0), env)};
    if (!track_addr) {
        track_addr = TrackLocalLoad(inst.Arg(0), block, env);
    }
    return track_addr;
}

TextureInst MakeInst(Environment& env, IR::Block* block, IR::Inst& inst) {
    ConstBufferAddr addr;
    if (IsBindless(inst)) {
        const std::optional<ConstBufferAddr> track_addr{TrackBindless(env, block, inst)};
        if (!track_addr) {
            throw NotImplementedException("Failed to track bindless texture constant buffer");
        }
        addr = *track_addr;
    } else {
        addr = ConstBufferAddr{
            .index = env.TextureBoundBuffer(),
            .offset = inst.Arg(0).U32(),
            .shift_left = 0,
            .secondary_index = 0,
            .secondary_offset = 0,
            .secondary_shift_left = 0,
            .dynamic_offset = {},
            .count = 1,
            .has_secondary = false,
        };
    }
    return TextureInst{
        .cbuf = addr,
        .inst = &inst,
        .block = block,
    };
}

u32 GetTextureHandle(Environment& env, const ConstBufferAddr& cbuf) {
    const u32 secondary_index{cbuf.has_secondary ? cbuf.secondary_index : cbuf.index};
    const u32 secondary_offset{cbuf.has_secondary ? cbuf.secondary_offset : cbuf.offset};
    const u32 lhs_raw{env.ReadCbufValue(cbuf.index, cbuf.offset) << cbuf.shift_left};
    const u32 rhs_raw{env.ReadCbufValue(secondary_index, secondary_offset)
                      << cbuf.secondary_shift_left};
    return lhs_raw | rhs_raw;
}

TextureType ReadTextureType(Environment& env, const ConstBufferAddr& cbuf) {
    return env.ReadTextureType(GetTextureHandle(env, cbuf));
}

TexturePixelFormat ReadTexturePixelFormat(Environment& env, const ConstBufferAddr& cbuf) {
    return env.ReadTexturePixelFormat(GetTextureHandle(env, cbuf));
}

bool IsTexturePixelFormatInteger(Environment& env, const ConstBufferAddr& cbuf) {
    return env.IsTexturePixelFormatInteger(GetTextureHandle(env, cbuf));
}

class Descriptors {
public:
    explicit Descriptors(TextureBufferDescriptors& texture_buffer_descriptors_,
                         ImageBufferDescriptors& image_buffer_descriptors_,
                         TextureDescriptors& texture_descriptors_,
                         ImageDescriptors& image_descriptors_)
        : texture_buffer_descriptors{texture_buffer_descriptors_},
          image_buffer_descriptors{image_buffer_descriptors_},
          texture_descriptors{texture_descriptors_}, image_descriptors{image_descriptors_} {}

    u32 Add(const TextureBufferDescriptor& desc) {
        return Add(texture_buffer_descriptors, desc, [&desc](const auto& existing) {
            return desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset &&
                   desc.shift_left == existing.shift_left &&
                   desc.secondary_cbuf_index == existing.secondary_cbuf_index &&
                   desc.secondary_cbuf_offset == existing.secondary_cbuf_offset &&
                   desc.secondary_shift_left == existing.secondary_shift_left &&
                   desc.count == existing.count && desc.size_shift == existing.size_shift &&
                   desc.has_secondary == existing.has_secondary;
        });
    }

    u32 Add(const ImageBufferDescriptor& desc) {
        const u32 index{Add(image_buffer_descriptors, desc, [&desc](const auto& existing) {
            return desc.format == existing.format && desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count &&
                   desc.size_shift == existing.size_shift;
        })};
        image_buffer_descriptors[index].is_written |= desc.is_written;
        image_buffer_descriptors[index].is_read |= desc.is_read;
        image_buffer_descriptors[index].is_integer |= desc.is_integer;
        return index;
    }

    u32 Add(const TextureDescriptor& desc) {
        const u32 index{Add(texture_descriptors, desc, [&desc](const auto& existing) {
            return desc.type == existing.type && desc.is_depth == existing.is_depth &&
                   desc.has_secondary == existing.has_secondary &&
                   desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset &&
                   desc.shift_left == existing.shift_left &&
                   desc.secondary_cbuf_index == existing.secondary_cbuf_index &&
                   desc.secondary_cbuf_offset == existing.secondary_cbuf_offset &&
                   desc.secondary_shift_left == existing.secondary_shift_left &&
                   desc.count == existing.count && desc.size_shift == existing.size_shift;
        })};
        // TODO: Read this from TIC
        texture_descriptors[index].is_multisample |= desc.is_multisample;
        return index;
    }

    u32 Add(const ImageDescriptor& desc) {
        const u32 index{Add(image_descriptors, desc, [&desc](const auto& existing) {
            return desc.type == existing.type && desc.format == existing.format &&
                   desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count &&
                   desc.size_shift == existing.size_shift;
        })};
        image_descriptors[index].is_written |= desc.is_written;
        image_descriptors[index].is_read |= desc.is_read;
        image_descriptors[index].is_integer |= desc.is_integer;
        return index;
    }

private:
    template <typename Descriptors, typename Descriptor, typename Func>
    static u32 Add(Descriptors& descriptors, const Descriptor& desc, Func&& pred) {
        // TODO: Handle arrays
        const auto it{std::ranges::find_if(descriptors, pred)};
        if (it != descriptors.end()) {
            return static_cast<u32>(std::distance(descriptors.begin(), it));
        }
        descriptors.push_back(desc);
        return static_cast<u32>(descriptors.size()) - 1;
    }

    TextureBufferDescriptors& texture_buffer_descriptors;
    ImageBufferDescriptors& image_buffer_descriptors;
    TextureDescriptors& texture_descriptors;
    ImageDescriptors& image_descriptors;
};

void PatchImageSampleImplicitLod(IR::Block& block, IR::Inst& inst) {
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    const auto info{inst.Flags<IR::TextureInstInfo>()};
    const IR::Value coord(inst.Arg(1));
    const IR::Value handle(ir.Imm32(0));
    const IR::U32 lod{ir.Imm32(0)};
    const IR::U1 skip_mips{ir.Imm1(true)};
    const IR::Value texture_size = ir.ImageQueryDimension(handle, lod, skip_mips, info);
    inst.SetArg(
        1, ir.CompositeConstruct(
               ir.FPMul(IR::F32(ir.CompositeExtract(coord, 0)),
                        ir.FPRecip(ir.ConvertUToF(32, 32, ir.CompositeExtract(texture_size, 0)))),
               ir.FPMul(IR::F32(ir.CompositeExtract(coord, 1)),
                        ir.FPRecip(ir.ConvertUToF(32, 32, ir.CompositeExtract(texture_size, 1))))));
}

bool IsPixelFormatSNorm(TexturePixelFormat pixel_format) {
    switch (pixel_format) {
    case TexturePixelFormat::A8B8G8R8_SNORM:
    case TexturePixelFormat::R8G8_SNORM:
    case TexturePixelFormat::R8_SNORM:
    case TexturePixelFormat::R16G16B16A16_SNORM:
    case TexturePixelFormat::R16G16_SNORM:
    case TexturePixelFormat::R16_SNORM:
        return true;
    default:
        return false;
    }
}

void PatchTexelFetch(IR::Block& block, IR::Inst& inst, TexturePixelFormat pixel_format) {
    const auto it{IR::Block::InstructionList::s_iterator_to(inst)};
    IR::IREmitter ir{block, IR::Block::InstructionList::s_iterator_to(inst)};
    auto get_max_value = [pixel_format]() -> float {
        switch (pixel_format) {
        case TexturePixelFormat::A8B8G8R8_SNORM:
        case TexturePixelFormat::R8G8_SNORM:
        case TexturePixelFormat::R8_SNORM:
            return 1.f / std::numeric_limits<char>::max();
        case TexturePixelFormat::R16G16B16A16_SNORM:
        case TexturePixelFormat::R16G16_SNORM:
        case TexturePixelFormat::R16_SNORM:
            return 1.f / std::numeric_limits<short>::max();
        default:
            throw InvalidArgument("Invalid texture pixel format");
        }
    };

    const IR::Value new_inst{&*block.PrependNewInst(it, inst)};
    const IR::F32 x(ir.CompositeExtract(new_inst, 0));
    const IR::F32 y(ir.CompositeExtract(new_inst, 1));
    const IR::F32 z(ir.CompositeExtract(new_inst, 2));
    const IR::F32 w(ir.CompositeExtract(new_inst, 3));
    const IR::F16F32F64 max_value(ir.Imm32(get_max_value()));
    const IR::Value converted =
        ir.CompositeConstruct(ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(x)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(y)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(z)), max_value),
                              ir.FPMul(ir.ConvertSToF(32, 32, ir.BitCast<IR::U32>(w)), max_value));
    inst.ReplaceUsesWith(converted);
}
} // Anonymous namespace

void TexturePass(Environment& env, IR::Program& program, const HostTranslateInfo& host_info) {
    TextureInstVector to_replace;
    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsTextureInstruction(inst)) {
                continue;
            }
            to_replace.push_back(MakeInst(env, block, inst));
        }
    }
    // All source expressions are still unchanged here. Share SSA results across accesses.
    std::unordered_set<const IR::Inst*> alignment_visiting;
    std::unordered_map<const IR::Inst*, u32> offset_alignments;
    for (auto& texture : to_replace) {
        if (texture.cbuf.count <= 1) continue;
        const u32 buffer_size = env.ReadCbufSize(texture.cbuf.index);
        if (buffer_size == 0) {
            throw NotImplementedException("Bindless constant buffer size is unavailable");
        }
        const u32 shift =
            DynamicDescriptorSizeShift(texture.cbuf, alignment_visiting, offset_alignments);
        texture.cbuf.size_shift = shift;
        // Enumerate the entire buffer at this field's alignment phase. Guest U32 additions
        // can wrap and select valid handles before the original immediate base.
        const u32 phase = texture.cbuf.offset & ((1U << shift) - 1);
        const u32 adjustment = texture.cbuf.offset - phase;
        if (adjustment != 0) {
            IR::IREmitter ir{*texture.block, IR::Block::InstructionList::s_iterator_to(*texture.inst)};
            texture.cbuf.dynamic_offset = IR::U32{ir.IAdd(texture.cbuf.dynamic_offset,
                                                         ir.Imm32(adjustment))};
        }
        texture.cbuf.offset = phase;
        texture.cbuf.count = BindlessDescriptorCount(buffer_size, phase, shift);
        if (texture.cbuf.count == 0) {
            throw NotImplementedException("Bindless constant buffer contains no complete handle");
        }
    }
    // Sort instructions to visit textures by constant buffer index, then by offset
    std::ranges::sort(to_replace, [](const auto& lhs, const auto& rhs) {
        return lhs.cbuf.offset < rhs.cbuf.offset;
    });
    std::stable_sort(to_replace.begin(), to_replace.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.cbuf.index < rhs.cbuf.index;
    });
    Descriptors descriptors{
        program.info.texture_buffer_descriptors,
        program.info.image_buffer_descriptors,
        program.info.texture_descriptors,
        program.info.image_descriptors,
    };
    for (TextureInst& texture_inst : to_replace) {
        // TODO: Handle arrays
        IR::Inst* const inst{texture_inst.inst};
        inst->ReplaceOpcode(IndexedInstruction(*inst));

        const auto& cbuf{texture_inst.cbuf};
        const u32 size_shift{cbuf.size_shift};
        auto flags{inst->Flags<IR::TextureInstInfo>()};
        bool is_multisample{false};
        switch (inst->GetOpcode()) {
        case IR::Opcode::ImageQueryDimensions:
            flags.type.Assign(ReadTextureType(env, cbuf));
            inst->SetFlags(flags);
            break;
        case IR::Opcode::ImageSampleImplicitLod:
            if (flags.type != TextureType::Color2D) {
                break;
            }
            if (ReadTextureType(env, cbuf) == TextureType::Color2DRect) {
                PatchImageSampleImplicitLod(*texture_inst.block, *texture_inst.inst);
            }
            break;
        case IR::Opcode::ImageFetch:
            if (flags.type == TextureType::Color2D || flags.type == TextureType::Color2DRect ||
                flags.type == TextureType::ColorArray2D) {
                is_multisample = !inst->Arg(4).IsEmpty();
            } else {
                inst->SetArg(4, IR::U32{});
            }
            if (flags.type != TextureType::Color1D) {
                break;
            }
            if (ReadTextureType(env, cbuf) == TextureType::Buffer) {
                // Replace with the bound texture type only when it's a texture buffer
                // If the instruction is 1D and the bound type is 2D, don't change the code and let
                // the rasterizer robustness handle it
                // This happens on Fire Emblem: Three Houses
                flags.type.Assign(TextureType::Buffer);
            }
            break;
        default:
            break;
        }
        u32 index;
        switch (inst->GetOpcode()) {
        case IR::Opcode::ImageRead:
        case IR::Opcode::ImageAtomicIAdd32:
        case IR::Opcode::ImageAtomicSMin32:
        case IR::Opcode::ImageAtomicUMin32:
        case IR::Opcode::ImageAtomicSMax32:
        case IR::Opcode::ImageAtomicUMax32:
        case IR::Opcode::ImageAtomicInc32:
        case IR::Opcode::ImageAtomicDec32:
        case IR::Opcode::ImageAtomicAnd32:
        case IR::Opcode::ImageAtomicOr32:
        case IR::Opcode::ImageAtomicXor32:
        case IR::Opcode::ImageAtomicExchange32:
        case IR::Opcode::ImageWrite: {
            if (cbuf.has_secondary) {
                throw NotImplementedException("Unexpected separate sampler");
            }
            const bool is_written{inst->GetOpcode() != IR::Opcode::ImageRead};
            const bool is_read{inst->GetOpcode() != IR::Opcode::ImageWrite};
            const bool is_integer{IsTexturePixelFormatInteger(env, cbuf)};
            if (flags.type == TextureType::Buffer) {
                index = descriptors.Add(ImageBufferDescriptor{
                    .format = flags.image_format,
                    .is_written = is_written,
                    .is_read = is_read,
                    .is_integer = is_integer,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .count = cbuf.count,
                    .size_shift = size_shift,
                });
            } else {
                index = descriptors.Add(ImageDescriptor{
                    .type = flags.type,
                    .format = flags.image_format,
                    .is_written = is_written,
                    .is_read = is_read,
                    .is_integer = is_integer,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .count = cbuf.count,
                    .size_shift = size_shift,
                });
            }
            break;
        }
        default:
            if (flags.type == TextureType::Buffer) {
                index = descriptors.Add(TextureBufferDescriptor{
                    .has_secondary = cbuf.has_secondary,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .shift_left = cbuf.shift_left,
                    .secondary_cbuf_index = cbuf.secondary_index,
                    .secondary_cbuf_offset = cbuf.secondary_offset,
                    .secondary_shift_left = cbuf.secondary_shift_left,
                    .count = cbuf.count,
                    .size_shift = size_shift,
                });
            } else {
                index = descriptors.Add(TextureDescriptor{
                    .type = flags.type,
                    .is_depth = flags.is_depth != 0,
                    .is_multisample = is_multisample,
                    .has_secondary = cbuf.has_secondary,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .shift_left = cbuf.shift_left,
                    .secondary_cbuf_index = cbuf.secondary_index,
                    .secondary_cbuf_offset = cbuf.secondary_offset,
                    .secondary_shift_left = cbuf.secondary_shift_left,
                    .count = cbuf.count,
                    .size_shift = size_shift,
                });
            }
            break;
        }
        flags.descriptor_index.Assign(index);
        inst->SetFlags(flags);

        if (cbuf.count > 1) {
            const auto insert_point{IR::Block::InstructionList::s_iterator_to(*inst)};
            IR::IREmitter ir{*texture_inst.block, insert_point};
            const IR::U32 shift{ir.Imm32(size_shift)};
            inst->SetArg(0, ir.UMin(ir.ShiftRightLogical(cbuf.dynamic_offset, shift),
                                    ir.Imm32(cbuf.count - 1)));
        } else {
            inst->SetArg(0, IR::Value{});
        }

        if (!host_info.support_snorm_render_buffer && inst->GetOpcode() == IR::Opcode::ImageFetch &&
            flags.type == TextureType::Buffer) {
            const auto pixel_format = ReadTexturePixelFormat(env, cbuf);
            if (IsPixelFormatSNorm(pixel_format)) {
                PatchTexelFetch(*texture_inst.block, *texture_inst.inst, pixel_format);
            }
        }
    }
    // Count actual bindings after deduplication, rather than texture instructions.
    // Never map a valid guest index to another handle to satisfy host resource limits.
    const u64 sampled_count = u64{NumDescriptors(program.info.texture_descriptors)} +
                              NumDescriptors(program.info.texture_buffer_descriptors);
    if (std::ranges::any_of(to_replace, [](const auto& texture) {
            return !texture.cbuf.dynamic_offset.IsEmpty();
        }) && sampled_count > host_info.max_bindless_descriptors_per_stage) {
        throw NotImplementedException("Sampled texture bindings exceed the host descriptor budget");
    }
}

void JoinTextureInfo(Info& base, Info& source) {
    Descriptors descriptors{
        base.texture_buffer_descriptors,
        base.image_buffer_descriptors,
        base.texture_descriptors,
        base.image_descriptors,
    };
    for (auto& desc : source.texture_buffer_descriptors) {
        descriptors.Add(desc);
    }
    for (auto& desc : source.image_buffer_descriptors) {
        descriptors.Add(desc);
    }
    for (auto& desc : source.texture_descriptors) {
        descriptors.Add(desc);
    }
    for (auto& desc : source.image_descriptors) {
        descriptors.Add(desc);
    }
}

} // namespace Shader::Optimization
