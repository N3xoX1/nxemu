// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <memory>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <boost/container/small_vector.hpp>

#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/ir_opt/conditional_value.h"
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
    bool is_direct{};
    u32 handle_or_bits{};
    u32 stride{};
    IR::U32 array_index;
    u32 mask{~0U};
    u32 secondary_mask{~0U};
    bool has_index_cbuf{};
    u32 index_cbuf_index{};
    u32 index_cbuf_offset{};
    u32 index_shift{};
    std::vector<TextureHandleTerm> extra_terms;
};

struct TextureInst {
    ConstBufferAddr cbuf;
    IR::Inst* inst;
    IR::Block* block;
    std::vector<TextureHandleSource> sources;
};

using TextureInstVector = boost::container::small_vector<TextureInst, 24>;

struct StridedIndex {
    IR::U32 index;
    u32 stride;
};

// Recover the actual element index for arrays of structures, for example
// FindUMsb(mask) * 304. Alignment alone would incorrectly interpret this as 19
// elements of a 16-byte array.
std::optional<StridedIndex> FindStridedIndex(const IR::U32& byte_offset) {
    const IR::Inst* const inst = byte_offset.TryInstRecursive();
    if (!inst) {
        return std::nullopt;
    }
    switch (inst->GetOpcode()) {
    case IR::Opcode::ShiftLeftLogical32:
        if (inst->Arg(1).IsImmediate()) {
            const u32 shift = inst->Arg(1).U32();
            if (shift >= 2 && shift <= 15) {
                return StridedIndex{IR::U32{inst->Arg(0)}, 1U << shift};
            }
        }
        break;
    case IR::Opcode::IMul32:
        for (size_t arg = 0; arg < 2; ++arg) {
            const IR::Value factor{inst->Arg(arg)};
            if (factor.IsImmediate() && factor.U32() >= sizeof(u32) &&
                (factor.U32() % sizeof(u32)) == 0 && factor.U32() <= 16 * 1024) {
                return StridedIndex{IR::U32{inst->Arg(1 - arg)}, factor.U32()};
            }
        }
        break;
    default:
        break;
    }
    return std::nullopt;
}

// Only fold an index known to be uniform throughout the draw. An arbitrary
// varying expression must keep the ordinary runtime descriptor-array path.
struct UniformHandleIndex {
    u32 cbuf_index;
    u32 cbuf_offset;
    u32 shift;
};

std::optional<UniformHandleIndex> TrackUniformHandleIndex(IR::U32 offset) {
    const IR::Inst* inst = offset.TryInstRecursive();
    if (!inst || inst->GetOpcode() != IR::Opcode::ShiftLeftLogical32 ||
        !inst->Arg(1).IsImmediate() || inst->Arg(1).U32() < 2 ||
        inst->Arg(1).U32() >= 32) {
        return std::nullopt;
    }
    const u32 shift = inst->Arg(1).U32();
    inst = inst->Arg(0).TryInstRecursive();
    if (!inst || inst->GetOpcode() != IR::Opcode::GetCbufU32 ||
        !inst->Arg(0).IsImmediate() || !inst->Arg(1).IsImmediate()) {
        return std::nullopt;
    }
    return UniformHandleIndex{inst->Arg(0).U32(), inst->Arg(1).U32(), shift};
}

// Exclusive upper bound, zero if the analysis cannot prove a finite bound.
u32 StridedIndexBound(const IR::U32& index, u32 base_offset, u32 stride, u32 buffer_size) {
    const IR::Value value{index.Resolve()};
    if (value.IsImmediate()) {
        return value.U32() == ~0U ? 0 : value.U32() + 1;
    }
    const IR::Inst* const inst = value.TryInstRecursive();
    if (!inst) {
        return 0;
    }
    switch (inst->GetOpcode()) {
    case IR::Opcode::FindUMsb32:
    case IR::Opcode::FindSMsb32:
        // FindMSB also returns -1. Its wrapped byte address must not refer to a
        // valid handle before the base; in that case retain byte-offset indexing.
        if (base_offset >= stride || u64{buffer_size} > (u64{1} << 32) - stride + base_offset) {
            return 0;
        }
        return 32;
    case IR::Opcode::BitwiseAnd32:
    case IR::Opcode::UMin32:
        for (size_t arg = 0; arg < 2; ++arg) {
            if (inst->Arg(arg).IsImmediate() && inst->Arg(arg).U32() < 16 * 1024) {
                return inst->Arg(arg).U32() + 1;
            }
        }
        break;
    default:
        break;
    }
    return 0;
}

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

class TextureHandleTracker;
std::optional<ConstBufferAddr> TryGetConstBuffer(const IR::Inst * inst, Environment & env,
                                                 TextureHandleTracker & tracker);
std::optional<u32> TryGetMaskOperand(const IR::Inst * inst, Environment & env, IR::Value & target);
std::optional<ConstBufferAddr> CombineHandles(ConstBufferAddr lhs, ConstBufferAddr rhs);
ConstBufferAddr ApplyHandleMask(ConstBufferAddr addr, u32 mask);
std::optional<ConstBufferAddr> ApplyHandleShiftLeft(ConstBufferAddr addr, u32 shift);
bool IsDynamicHandle(const ConstBufferAddr & addr);

// Evaluate the value expression, not the first cbuf reachable anywhere in its
// operands. In particular, Phi and Select inputs must agree unless the branch
// context proves which input is live. Cache failures as well as successes.
class TextureHandleTracker
{
public:
    TextureHandleTracker(Environment & env_, const IR::Program & program_, bool conditional_ = true) :
        env{env_}, program{program_}, conditions{program_}, conditional{conditional_}
    {
    }

    void BeginBlock(const IR::Block * block)
    {
        conditions.BeginBlock(block);
        cache.clear();
    }

    bool HasContradictoryUniformConditions()
    {
        return conditions.HasContradictoryUniformConditions();
    }

    bool InAlternatives() const noexcept
    {
        return in_alternatives;
    }

    std::optional<ConstBufferAddr> Track(IR::Value value)
    {
        value = value.Resolve();
        if (value.IsImmediate()) return std::nullopt;
        const auto * root = value.Inst();
        if (root->GetOpcode() == IR::Opcode::GetCbufU32 || root->GetOpcode() == IR::Opcode::GetCbufU32x2)
            return TryGetConstBuffer(root, env, *this);
        if (const auto it = cache.find(root); it != cache.end()) return it->second;
        // Explicit postorder avoids recursive walks and stack limits, including
        // loop-carried/cyclic SSA. An in-progress dependency stays unresolved.
        struct Work
        {
            const IR::Inst * inst;
            bool evaluate;
        };
        boost::container::small_vector<Work, 16> work{{root, false}};
        while (!work.empty())
        {
            const auto [inst, evaluate] = work.back();
            work.pop_back();
            if (evaluate)
            {
                cache.at(inst) = Evaluate(inst);
                continue;
            }
            if (!cache.emplace(inst, std::nullopt).second) continue;
            if (CrossesLoop(inst))
            {
                // The incoming expression can belong to a previous iteration.
                // Current branch facts cannot specialize that expression.
                const auto addr = Unconditional().Track(IR::Value{const_cast<IR::Inst *>(inst)});
                if (addr && addr->count == 1 && addr->dynamic_offset.IsEmpty()) cache.at(inst) = addr;
                continue;
            }
            work.push_back({inst, true});
            const auto add = [&](IR::Value operand) {
                operand = operand.Resolve();
                if (!operand.IsImmediate() && !cache.contains(operand.Inst()))
                {
                    work.push_back({operand.Inst(), false});
                }
            };
            switch (inst->GetOpcode())
            {
            case IR::Opcode::Phi:
                if (const auto input = InvariantPhiInput(inst); !input.IsEmpty())
                {
                    add(input);
                    break;
                }
                for (size_t i = 0; i < inst->NumArgs(); ++i)
                {
                    if (IncomingPossible(inst, i)) add(inst->Arg(i));
                }
                break;
            case IR::Opcode::SelectU32:
                if (const auto truth = Truth(inst->Arg(0)))
                    add(inst->Arg(*truth ? 1 : 2));
                else
                {
                    add(inst->Arg(1));
                    add(inst->Arg(2));
                }
                break;
            case IR::Opcode::BitwiseOr32:
            case IR::Opcode::BitwiseAnd32:
                add(inst->Arg(0));
                add(inst->Arg(1));
                break;
            case IR::Opcode::ShiftLeftLogical32:
                add(inst->Arg(0));
                break;
            default:
                break;
            }
        }
        return cache.at(root);
    }

    // Only finite Phi/Select/OR/AND/SHL graphs of proven scalar descriptors qualify.
    // Fixed-point evaluation handles loop-carried handle accumulation such as
    // `x = c1384; loop { if (p) x |= c416; }; use(x | c400)` without guessing.
    std::vector<ConstBufferAddr> Alternatives(IR::Value value, u32 limit)
    {
        if (limit == 0) return {};
        const auto root_value = value.Resolve();
        if (root_value.IsImmediate()) return {};
        const auto is_scalar = [](const ConstBufferAddr & addr) {
            return !addr.is_direct && !IsDynamicHandle(addr);
        };
        if (const auto direct = Track(root_value))
        {
            if (is_scalar(*direct)) return {*direct};
            return {};
        }

        const bool prev_in_alt = in_alternatives;
        in_alternatives = true;
        struct ScopeReset
        {
            bool & flag;
            bool previous;
            ~ScopeReset() { flag = previous; }
        } reset{in_alternatives, prev_in_alt};

        enum class NodeKind : u8
        {
            Leaf,
            Undef,
            Merge,
            Or,
            OrImm,
            AndMask,
            ShiftLeft,
        };
        struct AltNode
        {
            const IR::Inst * inst{};
            NodeKind kind{NodeKind::Leaf};
            u32 imm{};
            boost::container::small_vector<size_t, 4> deps;
            std::vector<ConstBufferAddr> values;
            std::unordered_set<ConstBufferAddr, AddressHash, AddressEqual> seen;
        };

        static constexpr size_t MAX_ALT_NODES = 64;
        static constexpr size_t MAX_ALT_ITERS = 64;
        const u32 effective_limit = std::min(limit, 64U);

        std::vector<AltNode> nodes;
        std::unordered_map<const IR::Inst *, size_t> node_map;
        std::vector<size_t> worklist;

        const auto intern = [&](IR::Value operand) -> std::optional<size_t> {
            operand = operand.Resolve();
            if (operand.IsImmediate()) return std::nullopt;
            const auto * inst = operand.Inst();
            if (const auto it = node_map.find(inst); it != node_map.end()) return it->second;
            if (nodes.size() >= MAX_ALT_NODES) return std::nullopt;
            const size_t idx = nodes.size();
            nodes.push_back(AltNode{.inst = inst});
            node_map.emplace(inst, idx);
            worklist.push_back(idx);
            return idx;
        };

        const auto add_val = [&](AltNode & node, const ConstBufferAddr & addr) -> bool {
            if (!is_scalar(addr)) return false;
            if (node.seen.insert(addr).second)
            {
                node.values.push_back(addr);
                return node.values.size() <= effective_limit;
            }
            return true;
        };

        if (!intern(root_value)) return {};

        while (!worklist.empty())
        {
            const size_t idx = worklist.back();
            worklist.pop_back();
            const IR::Inst * inst = nodes[idx].inst;

            if (CrossesLoop(inst))
            {
                // Keep the actual loop-carried handle as the runtime selector.
                // Only its finite, context-independent cbuf sources are collected.
                const auto incoming = Unconditional().Alternatives(IR::Value{const_cast<IR::Inst *>(inst)}, effective_limit);
                if (incoming.empty()) return {};
                nodes[idx].kind = NodeKind::Leaf;
                for (const auto & addr : incoming)
                    if (!add_val(nodes[idx], addr)) return {};
                continue;
            }
            if (const auto tracked = Track(IR::Value{const_cast<IR::Inst *>(inst)}))
            {
                nodes[idx].kind = NodeKind::Leaf;
                if (!add_val(nodes[idx], *tracked)) return {};
                continue;
            }

            switch (inst->GetOpcode())
            {
            case IR::Opcode::UndefU32:
                nodes[idx].kind = NodeKind::Undef;
                break;
            case IR::Opcode::Phi: {
                nodes[idx].kind = NodeKind::Merge;
                boost::container::small_vector<IR::Value, 8> inputs;
                for (size_t i = 0; i < inst->NumArgs(); ++i)
                {
                    if (!IncomingPossible(inst, i)) continue;
                    const IR::Value arg = inst->Arg(i).Resolve();
                    const auto * inner = arg.TryInstRecursive();
                    if (conditional && inner && inner != inst && inner->GetOpcode() == IR::Opcode::Phi)
                    {
                        bool pruned{};
                        boost::container::small_vector<IR::Value, 4> inner_inputs;
                        for (size_t j = 0; j < inner->NumArgs(); ++j)
                        {
                            if (!IncomingPossible(inner, j)) continue;
                            if (!conditions.IncomingPossibleThrough(inst, i, inner, j))
                            {
                                pruned = true;
                                continue;
                            }
                            inner_inputs.push_back(inner->Arg(j));
                        }
                        if (pruned)
                        {
                            for (const auto & v : inner_inputs) inputs.push_back(v);
                            continue;
                        }
                    }
                    inputs.push_back(arg);
                }
                for (const auto & input : inputs)
                {
                    const auto dep = intern(input);
                    if (!dep) return {};
                    nodes[idx].deps.push_back(*dep);
                }
                break;
            }
            case IR::Opcode::SelectU32: {
                nodes[idx].kind = NodeKind::Merge;
                if (const auto truth = Truth(inst->Arg(0)))
                {
                    const auto dep = intern(inst->Arg(*truth ? 1 : 2));
                    if (!dep) return {};
                    nodes[idx].deps.push_back(*dep);
                }
                else
                {
                    const auto dep1 = intern(inst->Arg(1));
                    const auto dep2 = intern(inst->Arg(2));
                    if (!dep1 || !dep2) return {};
                    nodes[idx].deps.push_back(*dep1);
                    nodes[idx].deps.push_back(*dep2);
                }
                break;
            }
            case IR::Opcode::BitwiseOr32: {
                bool handled_imm{};
                for (size_t imm_arg = 0; imm_arg < 2; ++imm_arg)
                {
                    if (inst->Arg(imm_arg).IsImmediate())
                    {
                        const u32 imm = inst->Arg(imm_arg).U32();
                        if ((imm & 0xfffffU) != 0) return {};
                        const auto dep = intern(inst->Arg(1 - imm_arg));
                        if (!dep) return {};
                        nodes[idx].kind = NodeKind::OrImm;
                        nodes[idx].imm = imm;
                        nodes[idx].deps.push_back(*dep);
                        handled_imm = true;
                        break;
                    }
                }
                if (!handled_imm)
                {
                    const auto dep0 = intern(inst->Arg(0));
                    const auto dep1 = intern(inst->Arg(1));
                    if (!dep0 || !dep1) return {};
                    nodes[idx].kind = NodeKind::Or;
                    nodes[idx].deps.push_back(*dep0);
                    nodes[idx].deps.push_back(*dep1);
                }
                break;
            }
            case IR::Opcode::BitwiseAnd32: {
                IR::Value target;
                const auto mask = TryGetMaskOperand(inst, env, target);
                if (!mask) return {};
                const auto dep = intern(target);
                if (!dep) return {};
                nodes[idx].kind = NodeKind::AndMask;
                nodes[idx].imm = *mask;
                nodes[idx].deps.push_back(*dep);
                break;
            }
            case IR::Opcode::ShiftLeftLogical32: {
                const IR::Value shift{inst->Arg(1)};
                if (!shift.IsImmediate() || shift.U32() >= 32) return {};
                const auto dep = intern(inst->Arg(0));
                if (!dep) return {};
                nodes[idx].kind = NodeKind::ShiftLeft;
                nodes[idx].imm = shift.U32();
                nodes[idx].deps.push_back(*dep);
                break;
            }
            default:
                return {}; // Not a finite merge/combination of known descriptors.
            }
        }

        bool converged{};
        for (size_t iter = 0; iter < MAX_ALT_ITERS; ++iter)
        {
            bool changed{};
            for (size_t rev = nodes.size(); rev-- > 0;)
            {
                auto & node = nodes[rev];
                const size_t before = node.values.size();
                switch (node.kind)
                {
                case NodeKind::Leaf:
                case NodeKind::Undef:
                    break;
                case NodeKind::Merge:
                    for (const size_t dep : node.deps)
                        for (const auto & addr : nodes[dep].values)
                            if (!add_val(node, addr)) return {};
                    break;
                case NodeKind::OrImm:
                    for (const auto & addr : nodes[node.deps[0]].values)
                    {
                        auto combined = addr;
                        combined.handle_or_bits |= node.imm;
                        if (!add_val(node, combined)) return {};
                    }
                    break;
                case NodeKind::Or:
                    for (const auto & lhs : nodes[node.deps[0]].values)
                        for (const auto & rhs : nodes[node.deps[1]].values)
                        {
                            const auto combined = CombineHandles(lhs, rhs);
                            if (!combined || !add_val(node, *combined)) return {};
                        }
                    break;
                case NodeKind::AndMask:
                    for (const auto & addr : nodes[node.deps[0]].values)
                        if (!add_val(node, ApplyHandleMask(addr, node.imm))) return {};
                    break;
                case NodeKind::ShiftLeft:
                    for (const auto & addr : nodes[node.deps[0]].values)
                    {
                        const auto shifted = ApplyHandleShiftLeft(addr, node.imm);
                        if (!shifted || !add_val(node, *shifted)) return {};
                    }
                    break;
                }
                if (node.values.size() != before) changed = true;
            }
            if (!changed)
            {
                converged = true;
                break;
            }
        }
        if (!converged || nodes[0].values.empty()) return {};

        // An arithmetic/bitwise op whose operand stayed empty (e.g. UndefU32 | c400)
        // combines uninitialized bits and must not silently disappear.
        for (const auto & node : nodes)
        {
            if (node.kind == NodeKind::Or || node.kind == NodeKind::OrImm ||
                node.kind == NodeKind::AndMask || node.kind == NodeKind::ShiftLeft)
            {
                for (const size_t dep : node.deps)
                    if (nodes[dep].values.empty()) return {};
            }
        }
        return std::move(nodes[0].values);
    }

private:
    bool CrossesLoop(const IR::Inst * inst)
    {
        return conditional && inst->GetOpcode() == IR::Opcode::Phi && conditions.CrossesLoop(inst);
    }

    TextureHandleTracker & Unconditional()
    {
        // No branch facts enter this cache, so it can be shared across use blocks.
        if (!unconditional) unconditional = std::make_unique<TextureHandleTracker>(env, program, false);
        return *unconditional;
    }

    bool IncomingPossible(const IR::Inst * phi, size_t index)
    {
        return !conditional || conditions.IncomingPossible(phi, index);
    }

    std::optional<bool> Truth(IR::Value value)
    {
        if (conditional) return conditions.Truth(value);
        value = value.Resolve();
        if (value.IsImmediate() && value.Type() == IR::Type::U1) return value.U1();
        return std::nullopt;
    }

    // A self edge carries the last iteration's value unchanged. If all other
    // defined edges carry the same SSA value, the phi is invariant without path
    // analysis. An undefined register can take that value too.
    static IR::Value InvariantPhiInput(const IR::Inst * phi)
    {
        IR::Value same;
        for (size_t i = 0; i < phi->NumArgs(); ++i)
        {
            const auto input = phi->Arg(i).Resolve();
            const auto * source = input.TryInstRecursive();
            if (source == phi || (source && source->GetOpcode() == IR::Opcode::UndefU32)) continue;
            if (!same.IsEmpty() && same != input) return {};
            same = input;
        }
        return same;
    }

public:
    static bool Same(const ConstBufferAddr & a, const ConstBufferAddr & b)
    {
        return a.index == b.index && a.offset == b.offset && a.shift_left == b.shift_left &&
               a.secondary_index == b.secondary_index && a.secondary_offset == b.secondary_offset &&
               a.secondary_shift_left == b.secondary_shift_left && a.dynamic_offset.Resolve() == b.dynamic_offset.Resolve() &&
               a.count == b.count && a.has_secondary == b.has_secondary &&
               a.is_direct == b.is_direct && a.handle_or_bits == b.handle_or_bits &&
               a.mask == b.mask && a.secondary_mask == b.secondary_mask &&
               a.has_index_cbuf == b.has_index_cbuf &&
               a.index_cbuf_index == b.index_cbuf_index &&
               a.index_cbuf_offset == b.index_cbuf_offset && a.index_shift == b.index_shift &&
               a.extra_terms == b.extra_terms;
    }

private:

    struct AddressHash
    {
        size_t operator()(const ConstBufferAddr & addr) const
        {
            size_t hash{};
            const auto mix = [&](u32 part) {
                hash ^= part + 0x9e3779b9U + (hash << 6) + (hash >> 2);
            };
            for (const u32 part : {addr.index, addr.offset, addr.shift_left, addr.secondary_index,
                                   addr.secondary_offset, addr.secondary_shift_left,
                                   u32{addr.has_secondary}, addr.handle_or_bits, addr.mask,
                                   addr.secondary_mask, u32{addr.has_index_cbuf},
                                   addr.index_cbuf_index, addr.index_cbuf_offset, addr.index_shift})
                mix(part);
            for (const auto & term : addr.extra_terms)
            {
                mix(term.cbuf_index);
                mix(term.cbuf_offset);
                mix(term.shift_left);
                mix(term.mask);
            }
            return hash;
        }
    };
    struct AddressEqual
    {
        bool operator()(const ConstBufferAddr & a, const ConstBufferAddr & b) const { return Same(a, b); }
    };

    std::optional<ConstBufferAddr> Evaluate(const IR::Inst * inst)
    {
        if (inst->GetOpcode() == IR::Opcode::Phi)
        {
            if (const auto input = InvariantPhiInput(inst); !input.IsEmpty()) return PhiResult(Track(input));
            std::optional<ConstBufferAddr> result;
            for (size_t i = 0; i < inst->NumArgs(); ++i)
            {
                if (!IncomingPossible(inst, i)) continue;
                const auto input = TrackIncoming(inst, i);
                if (!input || (result && !Same(*result, *input))) return std::nullopt;
                result = input;
            }
            return PhiResult(result);
        }
        if (inst->GetOpcode() == IR::Opcode::SelectU32)
        {
            if (const auto truth = Truth(inst->Arg(0))) return Track(inst->Arg(*truth ? 1 : 2));
            const auto yes = Track(inst->Arg(1));
            const auto no = Track(inst->Arg(2));
            return yes && no && Same(*yes, *no) ? yes : std::nullopt;
        }
        if (inst->GetOpcode() == IR::Opcode::CompositeExtractU32x2 && inst->Arg(1).IsImmediate())
        {
            const auto * source = inst->Arg(0).TryInstRecursive();
            if (source && source->GetOpcode() == IR::Opcode::GetCbufU32x2 && inst->Arg(1).U32() < 2 && source->Arg(1).IsImmediate())
            {
                auto addr = TryGetConstBuffer(source, env, *this);
                if (addr) addr->offset += inst->Arg(1).U32() * sizeof(u32);
                return addr;
            }
            return std::nullopt;
        }
        return TryGetConstBuffer(inst, env, *this);
    }

    // The value `phi` receives over its edge `index`. If that is a phi of the edge's
    // source block whose inputs disagree, drop only the inner edges the branch into
    // `phi` proves impossible. Every remaining input must still be the same handle.
    std::optional<ConstBufferAddr> TrackIncoming(const IR::Inst * phi, size_t index)
    {
        const IR::Value value = phi->Arg(index).Resolve();
        auto direct = Track(value);
        const auto * inner = value.TryInstRecursive();
        if (direct || !conditional || !inner || inner == phi || inner->GetOpcode() != IR::Opcode::Phi)
            return direct;
        std::optional<ConstBufferAddr> result;
        bool pruned{};
        for (size_t i = 0; i < inner->NumArgs(); ++i)
        {
            if (!IncomingPossible(inner, i)) continue;
            if (!conditions.IncomingPossibleThrough(phi, index, inner, i))
            {
                pruned = true;
                continue;
            }
            const auto input = Track(inner->Arg(i));
            if (!input || (result && !Same(*result, *input))) return std::nullopt;
            result = input;
        }
        return pruned ? PhiResult(result) : std::nullopt;
    }

    std::optional<ConstBufferAddr> PhiResult(std::optional<ConstBufferAddr> addr)
    {
        // The handle phi dominates its uses, but a dynamic offset taken from
        // only one incoming branch need not. Keep that choice in the guest handle
        // instead of emitting an SSA operand outside its defining branch.
        if (addr && !addr->dynamic_offset.IsEmpty() &&
            (!conditional || !conditions.DominatesUse(addr->dynamic_offset))) return std::nullopt;
        return addr;
    }

    Environment & env;
    const IR::Program & program;
    ConditionalValueResolver conditions;
    bool conditional;
    bool in_alternatives{};
    std::unique_ptr<TextureHandleTracker> unconditional;
    std::unordered_map<const IR::Inst *, std::optional<ConstBufferAddr>> cache;
};

std::optional<ConstBufferAddr> TrackLocalLoad(const IR::Value & value, const IR::Block * block,
                                              Environment & env, TextureHandleTracker & tracker)
{
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
    return TryGetConstBuffer(source, env, tracker);
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

constexpr size_t MAX_HANDLE_TERMS = 8;

bool IsDynamicHandle(const ConstBufferAddr& addr) {
    return addr.count > 1 || !addr.dynamic_offset.IsEmpty() || addr.has_index_cbuf ||
           !addr.array_index.IsEmpty();
}

bool TermLess(const TextureHandleTerm& a, const TextureHandleTerm& b) {
    return std::tie(a.shift_left, a.cbuf_index, a.cbuf_offset, a.mask) <
           std::tie(b.shift_left, b.cbuf_index, b.cbuf_offset, b.mask);
}

void CanonicalizeTerms(ConstBufferAddr& addr) {
    std::vector<TextureHandleTerm> terms;
    if (!IsDynamicHandle(addr)) {
        terms.push_back({addr.index, addr.offset, addr.shift_left, addr.mask});
        if (addr.has_secondary) {
            terms.push_back(
                {addr.secondary_index, addr.secondary_offset, addr.secondary_shift_left,
                 addr.secondary_mask});
        }
        terms.insert(terms.end(), addr.extra_terms.begin(), addr.extra_terms.end());
        std::ranges::sort(terms, TermLess);
        terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
        addr.index = terms[0].cbuf_index;
        addr.offset = terms[0].cbuf_offset;
        addr.shift_left = terms[0].shift_left;
        addr.mask = terms[0].mask;
        if (terms.size() >= 2) {
            addr.has_secondary = true;
            addr.secondary_index = terms[1].cbuf_index;
            addr.secondary_offset = terms[1].cbuf_offset;
            addr.secondary_shift_left = terms[1].shift_left;
            addr.secondary_mask = terms[1].mask;
            addr.extra_terms.assign(terms.begin() + 2, terms.end());
        } else {
            addr.has_secondary = false;
            addr.secondary_index = 0;
            addr.secondary_offset = 0;
            addr.secondary_shift_left = 0;
            addr.secondary_mask = ~0U;
            addr.extra_terms.clear();
        }
    } else if (addr.has_secondary) {
        terms.push_back({addr.secondary_index, addr.secondary_offset, addr.secondary_shift_left,
                         addr.secondary_mask});
        terms.insert(terms.end(), addr.extra_terms.begin(), addr.extra_terms.end());
        std::ranges::sort(terms, TermLess);
        terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
        addr.secondary_index = terms[0].cbuf_index;
        addr.secondary_offset = terms[0].cbuf_offset;
        addr.secondary_shift_left = terms[0].shift_left;
        addr.secondary_mask = terms[0].mask;
        addr.extra_terms.assign(terms.begin() + 1, terms.end());
    }
}

std::optional<ConstBufferAddr> CombineHandles(ConstBufferAddr lhs, ConstBufferAddr rhs) {
    if (lhs.is_direct || rhs.is_direct) {
        return std::nullopt;
    }
    const bool lhs_dyn = IsDynamicHandle(lhs);
    const bool rhs_dyn = IsDynamicHandle(rhs);
    if (lhs_dyn && rhs_dyn) {
        return std::nullopt;
    }
    // Only the primary component may be dynamically indexed. Shared sampler/extra
    // components always have fixed scalar offsets.
    if (rhs_dyn) {
        std::swap(lhs, rhs);
    }
    auto combined = lhs;
    combined.handle_or_bits |= rhs.handle_or_bits;
    std::vector<TextureHandleTerm> terms;
    if (IsDynamicHandle(combined)) {
        if (combined.has_secondary) {
            terms.push_back({combined.secondary_index, combined.secondary_offset,
                             combined.secondary_shift_left, combined.secondary_mask});
        }
        terms.insert(terms.end(), combined.extra_terms.begin(), combined.extra_terms.end());
        terms.push_back({rhs.index, rhs.offset, rhs.shift_left, rhs.mask});
        if (rhs.has_secondary) {
            terms.push_back({rhs.secondary_index, rhs.secondary_offset, rhs.secondary_shift_left,
                             rhs.secondary_mask});
        }
        terms.insert(terms.end(), rhs.extra_terms.begin(), rhs.extra_terms.end());
        std::ranges::sort(terms, TermLess);
        terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
        if (1 + terms.size() > MAX_HANDLE_TERMS) {
            return std::nullopt;
        }
        combined.has_secondary = true;
        combined.secondary_index = terms[0].cbuf_index;
        combined.secondary_offset = terms[0].cbuf_offset;
        combined.secondary_shift_left = terms[0].shift_left;
        combined.secondary_mask = terms[0].mask;
        combined.extra_terms.assign(terms.begin() + 1, terms.end());
        return combined;
    }
    terms.push_back({combined.index, combined.offset, combined.shift_left, combined.mask});
    if (combined.has_secondary) {
        terms.push_back({combined.secondary_index, combined.secondary_offset,
                         combined.secondary_shift_left, combined.secondary_mask});
    }
    terms.insert(terms.end(), combined.extra_terms.begin(), combined.extra_terms.end());
    terms.push_back({rhs.index, rhs.offset, rhs.shift_left, rhs.mask});
    if (rhs.has_secondary) {
        terms.push_back({rhs.secondary_index, rhs.secondary_offset, rhs.secondary_shift_left,
                         rhs.secondary_mask});
    }
    terms.insert(terms.end(), rhs.extra_terms.begin(), rhs.extra_terms.end());
    std::ranges::sort(terms, TermLess);
    terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
    if (terms.size() > MAX_HANDLE_TERMS) {
        return std::nullopt;
    }
    combined.index = terms[0].cbuf_index;
    combined.offset = terms[0].cbuf_offset;
    combined.shift_left = terms[0].shift_left;
    combined.mask = terms[0].mask;
    if (terms.size() >= 2) {
        combined.has_secondary = true;
        combined.secondary_index = terms[1].cbuf_index;
        combined.secondary_offset = terms[1].cbuf_offset;
        combined.secondary_shift_left = terms[1].shift_left;
        combined.secondary_mask = terms[1].mask;
        combined.extra_terms.assign(terms.begin() + 2, terms.end());
    } else {
        combined.has_secondary = false;
        combined.secondary_index = 0;
        combined.secondary_offset = 0;
        combined.secondary_shift_left = 0;
        combined.secondary_mask = ~0U;
        combined.extra_terms.clear();
    }
    return combined;
}

ConstBufferAddr ApplyHandleMask(ConstBufferAddr addr, u32 mask) {
    addr.mask &= mask;
    if (addr.has_secondary) {
        addr.secondary_mask &= mask;
    }
    for (auto& term : addr.extra_terms) {
        term.mask &= mask;
    }
    addr.handle_or_bits &= mask;
    CanonicalizeTerms(addr);
    return addr;
}

std::optional<ConstBufferAddr> ApplyHandleShiftLeft(ConstBufferAddr addr, u32 shift) {
    if (shift == 0) {
        return addr;
    }
    if (shift >= 32 || addr.has_secondary || !addr.extra_terms.empty() || addr.shift_left != 0) {
        return std::nullopt;
    }
    addr.shift_left = shift;
    addr.mask <<= shift;
    addr.handle_or_bits <<= shift;
    return addr;
}

std::optional<u32> TryGetMaskOperand(const IR::Inst* inst, Environment& env, IR::Value& target) {
    IR::Value op1{inst->Arg(0)};
    IR::Value op2{inst->Arg(1)};
    if (op1.IsImmediate()) {
        std::swap(op1, op2);
    }
    if (!op2.IsImmediate() && !op1.IsImmediate()) {
        if (const auto try_index = TryGetConstant(op1, env)) {
            target = op2;
            return *try_index;
        }
        if (const auto try_index_2 = TryGetConstant(op2, env)) {
            target = op1;
            return *try_index_2;
        }
        return std::nullopt;
    }
    if (op1.IsImmediate()) {
        return std::nullopt;
    }
    target = op1;
    return op2.U32();
}

// `x | c` where `x` is a merge whose every input combines with `c` to the same handle
// (for example a loop or branch that may already have merged the sampler word). Any
// distinct resulting handles leave this single-descriptor lookup unresolved so the
// compact multi-source selector can preserve all candidates without guessing.
std::optional<ConstBufferAddr> TrackRepeatedOr(TextureHandleTracker& tracker, IR::Value merged,
                                               const ConstBufferAddr& fixed, bool fixed_is_rhs) {
    if (fixed.is_direct || IsDynamicHandle(fixed)) {
        return std::nullopt;
    }
    static constexpr u32 MAX_INPUTS = 8;
    std::optional<ConstBufferAddr> result;
    for (const ConstBufferAddr& input : tracker.Alternatives(merged, MAX_INPUTS)) {
        const auto value = fixed_is_rhs ? CombineHandles(input, fixed) : CombineHandles(fixed, input);
        if (!value || (result && !TextureHandleTracker::Same(*result, *value))) {
            return std::nullopt;
        }
        result = value;
    }
    return result;
}

std::optional<ConstBufferAddr> TryGetConstBuffer(const IR::Inst* inst, Environment& env,
                                               TextureHandleTracker& tracker) {
    switch (inst->GetOpcode()) {
    default:
        return std::nullopt;
    case IR::Opcode::BitwiseOr32: {
        // TEX.B can combine a cbuf TIC handle with an immediate TSC index in bits 20..31.
        // Keep the constant in the descriptor instead of finding only the cbuf below the OR.
        for (size_t immediate = 0; immediate < 2; ++immediate) {
            if (inst->Arg(immediate).IsImmediate() &&
                (inst->Arg(immediate).U32() & 0xfffffU) == 0) {
                auto source = tracker.Track(inst->Arg(1 - immediate));
                if (source) {
                    source->handle_or_bits |= inst->Arg(immediate).U32();
                }
                return source;
            }
        }
        const std::optional lhs{tracker.Track(inst->Arg(0))};
        const std::optional rhs{tracker.Track(inst->Arg(1))};
        if (lhs && rhs) {
            return CombineHandles(*lhs, *rhs);
        }
        if ((lhs || rhs) && !tracker.InAlternatives()) {
            return TrackRepeatedOr(tracker, inst->Arg(lhs ? 1 : 0), lhs ? *lhs : *rhs, rhs.has_value());
        }
        return std::nullopt;
    }
    case IR::Opcode::ShiftLeftLogical32: {
        const IR::Value shift{inst->Arg(1)};
        if (!shift.IsImmediate() || shift.U32() >= 32) {
            return std::nullopt;
        }
        std::optional lhs{tracker.Track(inst->Arg(0))};
        if (lhs) {
            return ApplyHandleShiftLeft(*lhs, shift.U32());
        }
        return std::nullopt;
    }
    case IR::Opcode::BitwiseAnd32: {
        IR::Value target;
        const auto mask = TryGetMaskOperand(inst, env, target);
        if (!mask) {
            return std::nullopt;
        }
        std::optional lhs{tracker.Track(target)};
        if (lhs) {
            return ApplyHandleMask(*lhs, *mask);
        }
        return std::nullopt;
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
    u32 base_offset{};
    IR::U32 dynamic_offset;
    if (offset_inst->GetOpcode() != IR::Opcode::IAdd32) {
        // An indexed handle array can start at byte offset zero. Only accept a
        // proven index * constant expression; arbitrary offsets remain unsupported.
        if (!FindStridedIndex(IR::U32{offset})) {
            return std::nullopt;
        }
        dynamic_offset = IR::U32{offset};
    } else if (offset_inst->Arg(0).IsImmediate()) {
        base_offset = offset_inst->Arg(0).U32();
        dynamic_offset = IR::U32{offset_inst->Arg(1)};
    } else if (offset_inst->Arg(1).IsImmediate()) {
        base_offset = offset_inst->Arg(1).U32();
        dynamic_offset = IR::U32{offset_inst->Arg(0)};
    } else {
        return std::nullopt;
    }
    const auto uniform_index = TrackUniformHandleIndex(dynamic_offset);
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
        .has_index_cbuf = uniform_index.has_value(),
        .index_cbuf_index = uniform_index ? uniform_index->cbuf_index : 0,
        .index_cbuf_offset = uniform_index ? uniform_index->cbuf_offset : 0,
        .index_shift = uniform_index ? uniform_index->shift : 0,
    };
}

// Sampling instructions the backends can emit against separate image and sampler arrays.
bool IsRuntimeSampled(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::BindlessImageSampleImplicitLod:
    case IR::Opcode::BindlessImageSampleExplicitLod:
    case IR::Opcode::BindlessImageSampleDrefImplicitLod:
    case IR::Opcode::BindlessImageSampleDrefExplicitLod:
    case IR::Opcode::BindlessImageQueryLod:
    case IR::Opcode::BindlessImageGradient:
        return inst.Flags<IR::TextureInstInfo>().type != TextureType::Buffer;
    default:
        return false;
    }
}

std::optional<ConstBufferAddr> TrackBindless(Environment& env, IR::Block* block, IR::Inst& inst, TextureHandleTracker& tracker) {
    std::optional<ConstBufferAddr> track_addr{tracker.Track(inst.Arg(0))};
    if (!track_addr) {
        track_addr = TrackLocalLoad(inst.Arg(0), block, env, tracker);
    }
    return track_addr;
}

TextureInst MakeInst(Environment& env, IR::Block* block, IR::Inst& inst,
                     const HostTranslateInfo& host_info, TextureHandleTracker& tracker) {
    ConstBufferAddr addr;
    if (IsBindless(inst)) {
        const std::optional<ConstBufferAddr> track_addr{TrackBindless(env, block, inst, tracker)};
        if (!track_addr) {
            // An unresolved merge or runtime input has no single static descriptor.
            // Preserve the original handle and select its texture/sampler at runtime.
            const bool sampled = IsRuntimeSampled(inst);
            const bool fetch = inst.GetOpcode() == IR::Opcode::BindlessImageFetch;
            const bool gather = (inst.GetOpcode() == IR::Opcode::BindlessImageGather ||
                                 inst.GetOpcode() == IR::Opcode::BindlessImageGatherDref) &&
                                inst.Flags<IR::TextureInstInfo>().type != TextureType::Buffer;
            if (host_info.support_separate_sampler && (sampled || fetch || gather)) {
                // Keep finite merges compact. The original guest handle selects
                // among these cbuf sources, so no path is guessed and the full
                // guest TIC table does not need to be uploaded.
                auto choices = tracker.Alternatives(inst.Arg(0), host_info.max_bindless_descriptors_per_stage);
                if (choices.size() == 1) return TextureInst{choices.front(), &inst, block};
                if (!choices.empty()) {
                    IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                    IR::U32 selected = ir.Imm32(0);
                    std::vector<TextureHandleSource> sources;
                    sources.reserve(choices.size());
                    for (size_t i = 0; i < choices.size(); ++i) {
                        const auto& choice = choices[i];
                        sources.push_back({choice.has_secondary, choice.index, choice.offset, choice.shift_left,
                                           choice.secondary_index, choice.secondary_offset, choice.secondary_shift_left,
                                           1, 1U << choice.size_shift, choice.handle_or_bits,
                                           choice.mask, choice.secondary_mask, choice.extra_terms});
                        if (i == 0) continue; // The default index needs no handle comparison.
                        auto handle = ir.GetCbuf(ir.Imm32(choice.index), ir.Imm32(choice.offset));
                        if (choice.shift_left != 0) handle = ir.ShiftLeftLogical(handle, ir.Imm32(choice.shift_left));
                        if (choice.mask != ~0U) handle = ir.BitwiseAnd(handle, ir.Imm32(choice.mask));
                        if (choice.has_secondary) {
                            auto secondary = ir.GetCbuf(ir.Imm32(choice.secondary_index), ir.Imm32(choice.secondary_offset));
                            if (choice.secondary_shift_left != 0) secondary = ir.ShiftLeftLogical(secondary, ir.Imm32(choice.secondary_shift_left));
                            if (choice.secondary_mask != ~0U) secondary = ir.BitwiseAnd(secondary, ir.Imm32(choice.secondary_mask));
                            handle = ir.BitwiseOr(handle, secondary);
                        }
                        for (const auto& term : choice.extra_terms) {
                            auto extra = ir.GetCbuf(ir.Imm32(term.cbuf_index), ir.Imm32(term.cbuf_offset));
                            if (term.shift_left != 0) extra = ir.ShiftLeftLogical(extra, ir.Imm32(term.shift_left));
                            if (term.mask != ~0U) extra = ir.BitwiseAnd(extra, ir.Imm32(term.mask));
                            handle = ir.BitwiseOr(handle, extra);
                        }
                        if (choice.handle_or_bits != 0) handle = ir.BitwiseOr(handle, ir.Imm32(choice.handle_or_bits));
                        selected = IR::U32{ir.Select(ir.IEqual(IR::U32{inst.Arg(0)}, handle), ir.Imm32(static_cast<u32>(i)), selected)};
                    }
                    addr = choices.front();
                    addr.dynamic_offset = selected;
                    addr.count = static_cast<u32>(sources.size());
                    return TextureInst{addr, &inst, block, std::move(sources)};
                }
            }
            if (host_info.support_separate_sampler && sampled && env.ShaderStage() != Stage::Compute) {
                const auto pool = env.ReadTexturePoolInfo();
                if (pool && pool->num_textures != 0) {
                    // One element is not declared as an array.
                    addr = ConstBufferAddr{.dynamic_offset = IR::U32{inst.Arg(0)},
                                           .count = std::max(pool->num_textures, 2U),
                                           .is_direct = true};
                    return TextureInst{addr, &inst, block};
                }
            }
            // Handles passed through SSBOs/shared memory have no constant-buffer address.
            // Keep the runtime handle and index the TIC table directly instead of guessing
            // a cbuf from the address expression. Typeless 3D stores are the supported case.
            const auto flags = inst.Flags<IR::TextureInstInfo>();
            if (inst.GetOpcode() == IR::Opcode::BindlessImageWrite &&
                flags.type == TextureType::Color3D && flags.image_format == ImageFormat::Typeless) {
                const u32 size = env.ReadTextureTableSize();
                if (size != 0) {
                    addr = ConstBufferAddr{.dynamic_offset = IR::U32{inst.Arg(0)},
                                          .count = size + 1, .is_direct = true};
                    return TextureInst{addr, &inst, block};
                }
            }
            throw NotImplementedException("Failed to track bindless texture constant buffer\n{}",
                                          IR::DumpBlock(*block));
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

TextureHandleSource TextureQuerySource(const ConstBufferAddr& cbuf) {
    TextureHandleSource source{};
    source.has_secondary = cbuf.has_secondary;
    source.cbuf_index = cbuf.index;
    source.cbuf_offset = cbuf.offset;
    source.shift_left = cbuf.shift_left;
    source.secondary_cbuf_index = cbuf.secondary_index;
    source.secondary_cbuf_offset = cbuf.secondary_offset;
    source.secondary_shift_left = cbuf.secondary_shift_left;
    source.handle_or_bits = cbuf.handle_or_bits;
    source.mask = cbuf.mask;
    source.secondary_mask = cbuf.secondary_mask;
    source.extra_terms = cbuf.extra_terms;
    source.count = 1;
    return source;
}

TextureType ReadTextureType(Environment& env, const ConstBufferAddr& cbuf) {
    return static_cast<TextureType>(env.ReadTextureQuery(TextureQuerySource(cbuf), TextureQueryKind::Type));
}

bool SameStaticHandle(const ConstBufferAddr& lhs, const ConstBufferAddr& rhs) {
    return lhs.count == 1 && rhs.count == 1 && lhs.dynamic_offset.IsEmpty() &&
           rhs.dynamic_offset.IsEmpty() && !lhs.has_index_cbuf && !rhs.has_index_cbuf &&
           lhs.index == rhs.index && lhs.offset == rhs.offset && lhs.shift_left == rhs.shift_left &&
           lhs.has_secondary == rhs.has_secondary && lhs.is_direct == rhs.is_direct &&
           lhs.handle_or_bits == rhs.handle_or_bits && lhs.mask == rhs.mask &&
           (!lhs.has_secondary ||
            (lhs.secondary_index == rhs.secondary_index &&
             lhs.secondary_offset == rhs.secondary_offset &&
             lhs.secondary_shift_left == rhs.secondary_shift_left &&
             lhs.secondary_mask == rhs.secondary_mask)) &&
           lhs.extra_terms == rhs.extra_terms;
}

std::optional<TextureType> AgreedSiblingTextureType(const TextureInstVector& instructions,
                                                    const TextureInst& query) {
    std::optional<TextureType> sibling_type;
    for (const TextureInst& other : instructions) {
        if (other.inst == query.inst || !SameStaticHandle(query.cbuf, other.cbuf)) {
            continue;
        }
        const IR::Opcode bound{IndexedInstruction(*other.inst)};
        const IR::Opcode opcode{bound != IR::Opcode::Void ? bound : other.inst->GetOpcode()};
        if (opcode == IR::Opcode::ImageQueryDimensions || opcode == IR::Opcode::ImageQueryLod) {
            continue;
        }
        const TextureType type{other.inst->Flags<IR::TextureInstInfo>().type};
        if (opcode == IR::Opcode::ImageFetch && type == TextureType::Color1D) {
            continue;
        }
        if (type == TextureType::Buffer) {
            continue;
        }
        if (sibling_type && *sibling_type != type) {
            return std::nullopt;
        }
        sibling_type = type;
    }
    return sibling_type;
}

TexturePixelFormat ReadTexturePixelFormat(Environment& env, const ConstBufferAddr& cbuf) {
    return static_cast<TexturePixelFormat>(env.ReadTextureQuery(TextureQuerySource(cbuf), TextureQueryKind::Format));
}

bool IsTexturePixelFormatInteger(Environment& env, const ConstBufferAddr& cbuf) {
    return env.ReadTextureQuery(TextureQuerySource(cbuf), TextureQueryKind::Integer) != 0;
}

ImageFormat RawImageReadFormat(ImageFormat access, TexturePixelFormat texture) {
    // A narrow raw load still addresses a texel of the bound surface. Use a
    // bit-compatible integer view of that whole texel; SULD's destination
    // register count already selects the requested leading words. Shrinking
    // the view format changes the texel size and is invalid on Vulkan/Metal.
    if (access == ImageFormat::R32_UINT) {
        switch (texture) {
        case TexturePixelFormat::R32G32_FLOAT:
        case TexturePixelFormat::R32G32_UINT:
        case TexturePixelFormat::R32G32_SINT:
            return ImageFormat::R32G32_UINT;
        default:
            break;
        }
    }
    if (access == ImageFormat::R32_UINT || access == ImageFormat::R32G32_UINT) {
        switch (texture) {
        case TexturePixelFormat::R32G32B32A32_FLOAT:
        case TexturePixelFormat::R32G32B32A32_UINT:
        case TexturePixelFormat::R32G32B32A32_SINT:
            return ImageFormat::R32G32B32A32_UINT;
        default:
            break;
        }
    }
    return access;
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
                   desc.count == existing.count && desc.stride == existing.stride &&
                   desc.handle_or_bits == existing.handle_or_bits &&
                   desc.mask == existing.mask && desc.secondary_mask == existing.secondary_mask &&
                   desc.has_secondary == existing.has_secondary &&
                   desc.extra_terms == existing.extra_terms && desc.sources == existing.sources;
        });
    }

    u32 Add(const ImageBufferDescriptor& desc) {
        const u32 index{Add(image_buffer_descriptors, desc, [&desc](const auto& existing) {
            return desc.format == existing.format && desc.cbuf_index == existing.cbuf_index &&
                   desc.cbuf_offset == existing.cbuf_offset && desc.count == existing.count &&
                   desc.stride == existing.stride;
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
                   desc.count == existing.count && desc.stride == existing.stride &&
                   desc.handle_or_bits == existing.handle_or_bits &&
                   desc.mask == existing.mask && desc.secondary_mask == existing.secondary_mask &&
                   desc.has_index_cbuf == existing.has_index_cbuf &&
                   desc.index_cbuf_index == existing.index_cbuf_index &&
                   desc.index_cbuf_offset == existing.index_cbuf_offset &&
                   desc.index_shift == existing.index_shift &&
                   desc.is_direct == existing.is_direct &&
                   desc.extra_terms == existing.extra_terms && desc.sources == existing.sources;
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
                   desc.stride == existing.stride && desc.is_direct == existing.is_direct &&
                   (!desc.is_direct || (desc.is_integer == existing.is_integer &&
                                        desc.is_signed == existing.is_signed));
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
    TextureHandleTracker tracker{env, program};
    for (IR::Block* const block : program.post_order_blocks) {
        tracker.BeginBlock(block);
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsTextureInstruction(inst)) {
                continue;
            }
            // An impossible uniform branch can contain a stale loop-carried
            // register as a texture handle. It never fetches at runtime, so do
            // not allocate an entire TIC table just to compile that dead fetch.
            if (inst.GetOpcode() == IR::Opcode::BindlessImageFetch &&
                tracker.HasContradictoryUniformConditions()) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                const auto zero = ir.Imm32(0.0f);
                inst.ReplaceUsesWith(ir.CompositeConstruct(zero, zero, zero, zero));
                continue;
            }
            to_replace.push_back(MakeInst(env, block, inst, host_info, tracker));
        }
    }
    // All source expressions are still unchanged here. Share SSA results across accesses.
    std::unordered_set<const IR::Inst*> alignment_visiting;
    std::unordered_map<const IR::Inst*, u32> offset_alignments;
    for (auto& texture : to_replace) {
        if (texture.cbuf.count <= 1 || texture.cbuf.is_direct || !texture.sources.empty()) continue;
        const u32 buffer_size = env.ReadCbufSize(texture.cbuf.index);
        if (buffer_size == 0) {
            throw NotImplementedException("Bindless constant buffer size is unavailable");
        }
        if (texture.cbuf.has_index_cbuf &&
            texture.inst->Flags<IR::TextureInstInfo>().type != TextureType::Buffer &&
            texture.cbuf.index_cbuf_offset % sizeof(u32) == 0 &&
            buffer_size >= sizeof(u32) && texture.cbuf.offset <= buffer_size - sizeof(u32) &&
            env.ReadCbufSize(texture.cbuf.index_cbuf_index) >= sizeof(u32) &&
            texture.cbuf.index_cbuf_offset <=
                env.ReadCbufSize(texture.cbuf.index_cbuf_index) - sizeof(u32)) {
            // One shader descriptor, selected from the guest cbuf at each draw.
            // No large host descriptor array and no per-fragment index calculation.
            texture.cbuf.count = 1;
            continue;
        }
        // Not proven uniform or in range: keep the existing indexed-array path.
        texture.cbuf.has_index_cbuf = false;
        // Prefer the actual structure stride when the guest offset is index * constant.
        // This also prevents enumerating thousands of false handles for 192/304-byte records.
        if (const auto strided = FindStridedIndex(texture.cbuf.dynamic_offset)) {
            const u32 bound = StridedIndexBound(strided->index, texture.cbuf.offset,
                                               strided->stride, buffer_size);
            // Unbounded U32 products can wrap back into the buffer. Only replace
            // byte offsets by element indices when the reachable products fit.
            if (bound != 0 && u64{texture.cbuf.offset} +
                                  u64{bound - 1} * strided->stride <= ~0U) {
                const u32 count = std::min(bound, BindlessDescriptorCountStrided(
                    buffer_size, texture.cbuf.offset, strided->stride));
                if (count == 0) {
                    throw NotImplementedException("Indexed handle is outside the constant buffer");
                }
                texture.cbuf.array_index = strided->index;
                texture.cbuf.stride = strided->stride;
                texture.cbuf.count = count;
                continue;
            }
        }
        const u32 shift =
            DynamicDescriptorSizeShift(texture.cbuf, alignment_visiting, offset_alignments);
        texture.cbuf.size_shift = shift;
        texture.cbuf.stride = 1U << shift;
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
        const u32 stride{cbuf.stride != 0 ? cbuf.stride : DESCRIPTOR_SIZE};
        auto flags{inst->Flags<IR::TextureInstInfo>()};
        bool is_multisample{false};
        switch (inst->GetOpcode()) {
        case IR::Opcode::ImageQueryDimensions:
            if (const auto sibling{AgreedSiblingTextureType(to_replace, texture_inst)}) {
                flags.type.Assign(*sibling);
            } else {
                flags.type.Assign(ReadTextureType(env, cbuf));
            }
            inst->SetFlags(flags);
            break;
        case IR::Opcode::ImageSampleImplicitLod:
            if (flags.type != TextureType::Color2D || cbuf.is_direct) {
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
            if (cbuf.has_secondary || !cbuf.extra_terms.empty()) {
                throw NotImplementedException("Unexpected separate sampler");
            }
            const bool is_written{inst->GetOpcode() != IR::Opcode::ImageRead};
            const bool is_read{inst->GetOpcode() != IR::Opcode::ImageWrite};
            if (!cbuf.is_direct && inst->GetOpcode() == IR::Opcode::ImageRead &&
                flags.type != TextureType::Buffer && flags.image_format != ImageFormat::Typeless) {
                flags.image_format.Assign(RawImageReadFormat(flags.image_format,
                                                            ReadTexturePixelFormat(env, cbuf)));
            }
            // Explicit formats describe raw integer accesses, even when the
            // original surface stores floating-point texels.
            const bool is_integer{flags.image_format != ImageFormat::Typeless ||
                                  (!cbuf.is_direct && IsTexturePixelFormatInteger(env, cbuf))};
            if (flags.type == TextureType::Buffer) {
                index = descriptors.Add(ImageBufferDescriptor{
                    .format = flags.image_format,
                    .is_written = is_written,
                    .is_read = is_read,
                    .is_integer = is_integer,
                    .cbuf_index = cbuf.index,
                    .cbuf_offset = cbuf.offset,
                    .count = cbuf.count,
                    .stride = stride,
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
                    .stride = stride,
                    .is_direct = cbuf.is_direct,
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
                    .stride = stride,
                    .handle_or_bits = cbuf.handle_or_bits,
                    .mask = cbuf.mask,
                    .secondary_mask = cbuf.secondary_mask,
                    .extra_terms = cbuf.extra_terms,
                    .sources = texture_inst.sources,
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
                    .stride = stride,
                    .is_direct = cbuf.is_direct,
                    .handle_or_bits = cbuf.handle_or_bits,
                    .has_index_cbuf = cbuf.has_index_cbuf,
                    .index_cbuf_index = cbuf.index_cbuf_index,
                    .index_cbuf_offset = cbuf.index_cbuf_offset,
                    .index_shift = cbuf.index_shift,
                    .mask = cbuf.mask,
                    .secondary_mask = cbuf.secondary_mask,
                    .extra_terms = cbuf.extra_terms,
                    .sources = texture_inst.sources,
                });
            }
            break;
        }
        flags.descriptor_index.Assign(index);
        inst->SetFlags(flags);

        if (!texture_inst.sources.empty()) {
            inst->SetArg(0, cbuf.dynamic_offset);
        } else if (cbuf.is_direct && inst->GetOpcode() != IR::Opcode::ImageWrite) {
            const TexturePoolInfo pool{env.ReadTexturePoolInfo().value()};
            const u32 num_samplers{pool.linked_samplers ? cbuf.count
                                                        : std::max(pool.num_samplers, 2U)};
            program.info.runtime_sampler_count = num_samplers;
            program.info.runtime_samplers_linked = pool.linked_samplers;
            IR::IREmitter ir{*texture_inst.block, IR::Block::InstructionList::s_iterator_to(*inst)};
            const IR::U32 handle{cbuf.dynamic_offset};
            // Out of range indices select the last element instead of reading past the arrays.
            if (pool.linked_samplers) {
                inst->SetArg(0, ir.UMin(handle, ir.Imm32(cbuf.count - 1)));
            } else {
                // Keep the guest layout: texture index in the low 20 bits, sampler index above.
                const IR::U32 tic{ir.UMin(ir.BitwiseAnd(handle, ir.Imm32(0xfffffU)),
                                          ir.Imm32(cbuf.count - 1))};
                const IR::U32 tsc{ir.UMin(ir.ShiftRightLogical(handle, ir.Imm32(20)),
                                          ir.Imm32(num_samplers - 1))};
                inst->SetArg(0, ir.BitwiseOr(tic, ir.ShiftLeftLogical(tsc, ir.Imm32(20))));
            }
        } else if (cbuf.is_direct) {
            if (!host_info.support_null_descriptor) {
                throw NotImplementedException("Runtime storage image table requires null descriptors");
            }
            program.info.runtime_image_write_mask_words = (cbuf.count + 31U) / 32U;
            IR::IREmitter ir{*texture_inst.block, IR::Block::InstructionList::s_iterator_to(*inst)};
            const auto tic_index = ir.BitwiseAnd(cbuf.dynamic_offset, ir.Imm32(0xfffffU));
            // The final descriptor is a null image for handles outside the bound table.
            inst->SetArg(0, ir.UMin(tic_index, ir.Imm32(cbuf.count - 1)));
            // A runtime TIC can select float, unsigned or signed storage. Bind the
            // matching view in one typed table and null descriptors in the other two.
            // nullDescriptor discards their stores without interpreting the texel bits.
            auto typed_desc = program.info.image_descriptors[index];
            typed_desc.is_integer = true;
            for (const bool is_signed : {false, true}) {
                typed_desc.is_signed = is_signed;
                auto typed_flags = flags;
                typed_flags.descriptor_index.Assign(descriptors.Add(typed_desc));
                auto clone = texture_inst.block->PrependNewInst(
                    IR::Block::InstructionList::s_iterator_to(*inst), *inst);
                clone->SetFlags(typed_flags);
            }
        } else if (cbuf.count > 1) {
            const auto insert_point{IR::Block::InstructionList::s_iterator_to(*inst)};
            IR::IREmitter ir{*texture_inst.block, insert_point};
            const IR::U32 element{!cbuf.array_index.IsEmpty()
                                      ? cbuf.array_index
                                      : IR::U32{ir.ShiftRightLogical(
                                            cbuf.dynamic_offset,
                                            ir.Imm32(cbuf.size_shift))}};
            inst->SetArg(0, ir.UMin(element, ir.Imm32(cbuf.count - 1)));
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
                              NumDescriptors(program.info.texture_buffer_descriptors) +
                              program.info.runtime_sampler_count;
    if (std::ranges::any_of(to_replace, [](const auto& texture) {
            return !texture.cbuf.dynamic_offset.IsEmpty();
        }) && sampled_count > host_info.max_bindless_descriptors_per_stage) {
        throw NotImplementedException("Sampled texture bindings exceed the host descriptor budget ({} > {})",
                                      sampled_count, host_info.max_bindless_descriptors_per_stage);
    }
    if (NumDescriptors(program.info.image_descriptors) > host_info.max_storage_images_per_stage &&
        std::ranges::any_of(program.info.image_descriptors, [](const auto& desc) { return desc.is_direct; })) {
        throw NotImplementedException("Runtime storage image table exceeds the host descriptor budget");
    }
}

void JoinTextureInfo(Info& base, Info& source) {
    Descriptors descriptors{
        base.texture_buffer_descriptors,
        base.image_buffer_descriptors,
        base.texture_descriptors,
        base.image_descriptors,
    };
    base.runtime_sampler_count =
        std::max(base.runtime_sampler_count, source.runtime_sampler_count);
    base.runtime_samplers_linked |= source.runtime_samplers_linked;
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
