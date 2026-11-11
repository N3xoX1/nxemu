/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <mcl/bitsizeof.hpp>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>

#include "dynarmic/backend/exception_handler.h"
#include "dynarmic/backend/x64/reg_alloc.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/ir/location_descriptor.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::IR {
class Block;
class Inst;
}  // namespace Dynarmic::IR

namespace Dynarmic {
enum class OptimizationFlag : u32;
}  // namespace Dynarmic

namespace Dynarmic::Backend::X64 {

class BlockOfCode;

using A64FullVectorWidth = std::integral_constant<size_t, 128>;

// Array alias that always sizes itself according to the given type T
// relative to the size of a vector register. e.g. T = u32 would result
// in a std::array<u32, 4>.
template<typename T>
using VectorArray = std::array<T, A64FullVectorWidth::value / mcl::bitsizeof<T>>;

template<typename T>
using HalfVectorArray = std::array<T, A64FullVectorWidth::value / mcl::bitsizeof<T> / 2>;

struct EmitContext {
    EmitContext(RegAlloc& reg_alloc, IR::Block& block);
    virtual ~EmitContext();

    void EraseInstruction(IR::Inst* inst);

    virtual FP::FPCR FPCR(bool fpcr_controlled = true) const = 0;

    virtual bool HasOptimization(OptimizationFlag flag) const = 0;

    RegAlloc& reg_alloc;
    IR::Block& block;

    std::vector<std::function<void()>> deferred_emits;

    /// NZCV value that the host flags hold right now (SF, ZF, CF = guest C, OF), or null
    const IR::Inst* host_flags_nzcv = nullptr;
    /// The host flags hold the same NZCV as cpsr_nzcv, so a condition needs no reload
    bool host_flags_are_cpsr = false;
    /// IsZero whose only use is the side exit right after it: ZF holds the result, the
    /// value itself is never written
    const IR::Inst* zero_flag_for = nullptr;

    /// Fastmem writes whose write tracking mark is done by a later write of the same group
    tsl::robin_set<const IR::Inst*> write_track_skip;
    /// Last write of a group -> offsets from its address to the lowest and highest address
    /// of the group, both marked by that write
    tsl::robin_map<const IR::Inst*, std::pair<s32, s32>> write_track_group;
};

using SharedLabel = std::shared_ptr<Xbyak::Label>;

inline SharedLabel GenSharedLabel() {
    return std::make_shared<Xbyak::Label>();
}

/// True when the value is known to have every bit above `kept_bits` clear, wherever it lives:
/// the loads below and the zero extensions clear them in an XMM, and a GPR value moved to an
/// XMM later goes through movd/movq, which clears them too.
inline bool UpperLanesClear(const IR::Value& value, size_t kept_bits) {
    if (value.IsImmediate()) {
        return false;
    }
    switch (value.GetInstRecursive()->GetOpcode()) {
    case IR::Opcode::A64GetS:
    case IR::Opcode::ZeroExtendWordToLong:
        return kept_bits >= 32;
    case IR::Opcode::A64GetD:
    case IR::Opcode::ZeroExtendLongToQuad:
        return kept_bits >= 64;
    default:
        return false;
    }
}

/// True when the value, held in a GPR, already has its upper 32 bits clear: the instruction
/// that produced it writes a 32-bit register, which zero extends on x64.
inline bool GprZeroExtended32(const IR::Value& value) {
    if (value.IsImmediate()) {
        return false;
    }
    switch (value.GetInstRecursive()->GetOpcode()) {
    case IR::Opcode::A64GetW:
    case IR::Opcode::A64ReadMemory32:
    case IR::Opcode::A64ExclusiveReadMemory32:
    case IR::Opcode::A64ExclusiveWriteMemory8:
    case IR::Opcode::A64ExclusiveWriteMemory16:
    case IR::Opcode::A64ExclusiveWriteMemory32:
    case IR::Opcode::A64ExclusiveWriteMemory64:
    case IR::Opcode::A64ExclusiveWriteMemory128:
    case IR::Opcode::ZeroExtendByteToWord:
    case IR::Opcode::ZeroExtendHalfToWord:
    case IR::Opcode::SignExtendByteToWord:
    case IR::Opcode::SignExtendHalfToWord:
    case IR::Opcode::ConditionalSelect32:
    case IR::Opcode::Add32:
    case IR::Opcode::Sub32:
    case IR::Opcode::And32:
    case IR::Opcode::AndNot32:
    case IR::Opcode::Or32:
        return true;
    default:
        return false;
    }
}

class EmitX64 {
public:
    struct BlockDescriptor {
        CodePtr entrypoint;  // Entrypoint of emitted code
        size_t size;         // Length in bytes of emitted code
    };

    explicit EmitX64(BlockOfCode& code);
    virtual ~EmitX64();

    /// Looks up an emitted host block in the cache.
    std::optional<BlockDescriptor> GetBasicBlock(IR::LocationDescriptor descriptor) const;

    /// Empties the entire cache.
    virtual void ClearCache();

    /// Invalidates a selection of basic blocks.
    void InvalidateBasicBlocks(const tsl::robin_set<IR::LocationDescriptor>& locations);

    /// Guards the block maps when several cores share this code: held shared for lookups and
    /// exclusive to compile, link, invalidate or handle a fastmem fault.
    std::shared_mutex cache_mutex;

protected:
    // Microinstruction emitters
#define OPCODE(name, type, ...) void Emit##name(EmitContext& ctx, IR::Inst* inst);
#define A32OPC(...)
#define A64OPC(...)
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC

    // Helpers
    virtual std::string LocationDescriptorToFriendlyName(const IR::LocationDescriptor&) const = 0;
    void EmitAddCycles(size_t cycles);
    Xbyak::Label EmitCond(IR::Cond cond);
    BlockDescriptor RegisterBlock(const IR::LocationDescriptor& location_descriptor, CodePtr entrypoint, size_t size);
    void PushRSBHelper(Xbyak::Reg64 loc_desc_reg, Xbyak::Reg64 index_reg, IR::LocationDescriptor target);

    void EmitVerboseDebuggingOutput(RegAlloc& reg_alloc);

    // Terminal instruction emitters
    void EmitTerminal(IR::Terminal terminal, IR::LocationDescriptor initial_location, bool is_single_step);
    virtual void EmitTerminalImpl(IR::Term::Interpret terminal, IR::LocationDescriptor initial_location, bool is_single_step) = 0;
    virtual void EmitTerminalImpl(IR::Term::ReturnToDispatch terminal, IR::LocationDescriptor initial_location, bool is_single_step) = 0;
    virtual void EmitTerminalImpl(IR::Term::LinkBlock terminal, IR::LocationDescriptor initial_location, bool is_single_step) = 0;
    virtual void EmitTerminalImpl(IR::Term::LinkBlockFast terminal, IR::LocationDescriptor initial_location, bool is_single_step) = 0;
    virtual void EmitTerminalImpl(IR::Term::PopRSBHint terminal, IR::LocationDescriptor initial_location, bool is_single_step) = 0;
    virtual void EmitTerminalImpl(IR::Term::FastDispatchHint terminal, IR::LocationDescriptor initial_location, bool is_single_step) = 0;
    virtual void EmitTerminalImpl(IR::Term::If terminal, IR::LocationDescriptor initial_location, bool is_single_step) = 0;
    virtual void EmitTerminalImpl(IR::Term::CheckBit terminal, IR::LocationDescriptor initial_location, bool is_single_step) = 0;
    virtual void EmitTerminalImpl(IR::Term::CheckHalt terminal, IR::LocationDescriptor initial_location, bool is_single_step) = 0;

    // Patching
    struct PatchInformation {
        std::vector<CodePtr> jg;
        std::vector<CodePtr> jz;
        std::vector<CodePtr> jmp;
        std::vector<CodePtr> mov_rcx;
    };
    virtual void Patch(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr);
    virtual void Unpatch(const IR::LocationDescriptor& target_desc);
    virtual void EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) = 0;
    virtual void EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) = 0;
    virtual void EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) = 0;
    virtual void EmitPatchMovRcx(CodePtr target_code_ptr = nullptr) = 0;

    // State
    BlockOfCode& code;
    ExceptionHandler exception_handler;
    tsl::robin_map<IR::LocationDescriptor, BlockDescriptor> block_descriptors;
    tsl::robin_map<IR::LocationDescriptor, PatchInformation> patch_information;
};

}  // namespace Dynarmic::Backend::X64
