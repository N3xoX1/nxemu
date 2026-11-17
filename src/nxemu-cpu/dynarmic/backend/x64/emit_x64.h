/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <mutex>
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

    /// Host flags may be used across IR instructions: side exits, CSEL and FCMP on live flags,
    /// IsZero fused into the side exit. Off for A32, for test variant bit 8 and with verbose
    /// debugging output, which changes the flags between instructions.
    bool reuse_host_flags = false;
    /// NZCV value that the host flags hold right now (SF, ZF, CF = guest C, OF), or null
    const IR::Inst* host_flags_nzcv = nullptr;
    /// The host flags hold the same NZCV as cpsr_nzcv, so a condition needs no reload
    bool host_flags_are_cpsr = false;
    /// host_flags_nzcv is an FPCompare: the host flags are those of ucomis (ZF, PF, CF), not
    /// the guest NZCV layout, so conditions map to other jumps
    bool host_flags_fcmp = false;
    /// IsZero whose only use is the side exit right after it: ZF holds the result, the
    /// value itself is never written
    const IR::Inst* zero_flag_for = nullptr;
    /// The block ends in a CheckBit exit that can test the host flags (a top-level CheckBit, no
    /// cycle count update before it): an IsZero feeding the block's last instruction,
    /// SetCheckBit, then only sets ZF
    bool check_bit_exit_on_flags = false;
    /// GetNZCVFromOp values kept in the host flags only. Their SetNZCV became A64SetNZCVAtExits
    /// (the A64 emitter's PlanNZCVAtExits): the producer leaves NZCV in the host flags, and the
    /// side exits and the block end store it.
    std::vector<std::pair<const IR::Inst*, const IR::Inst*>> nzcv_at_exits;  // {SetNZCVAtExits, GetNZCVFromOp}
    bool IsNzcvInFlagsOnly(const IR::Inst* get_nzcv) const {
        for (const auto& [set, get] : nzcv_at_exits) {
            if (get == get_nzcv) {
                return true;
            }
        }
        return false;
    }
    /// The guest NZCV is in the host flags and not yet in cpsr_nzcv
    bool nzcv_pending = false;
};

using SharedLabel = std::shared_ptr<Xbyak::Label>;

inline SharedLabel GenSharedLabel() {
    return std::make_shared<Xbyak::Label>();
}

/// Test variant bit 16: the code generation before zero extension tracking, adc for FCMP NZCV
/// and plain ordered loads, for differential tests
inline bool BaselineCodegen() {
    return (jit_test_variant & 16) != 0;
}

/// Test variant bit 256: the code generation before 64-bit immediates for ADD/SUB/CMP, plain
/// SetS/SetD stores, 32/64-bit vector loads into XMM registers, lane inserts from an XMM register
/// and the zero extension of LeastSignificantWord, MostSignificantWord and loads, for
/// differential tests
inline bool BaselineLoadsAndImmediates() {
    return (jit_test_variant & 256) != 0;
}

/// True when the value is known to have every bit above `kept_bits` clear, wherever it lives:
/// the loads below and the zero extensions clear them in an XMM, and a GPR value moved to an
/// XMM later goes through movd/movq, which clears them too.
inline bool UpperLanesClear(const IR::Value& value, size_t kept_bits) {
    if (value.IsImmediate() || BaselineCodegen()) {
        return false;
    }
    switch (value.GetInstRecursive()->GetOpcode()) {
    case IR::Opcode::A64GetS:
    case IR::Opcode::ZeroExtendWordToLong:
        return kept_bits >= 32;
    case IR::Opcode::A64GetD:
    case IR::Opcode::ZeroExtendLongToQuad:
        return kept_bits >= 64;
    case IR::Opcode::A64ReadMemory32:  // a zero-extending GPR load, or movd into an XMM
        return kept_bits >= 32 && !BaselineLoadsAndImmediates();
    case IR::Opcode::A64ReadMemory64:  // a GPR load, or movq into an XMM
        return kept_bits >= 64 && !BaselineLoadsAndImmediates();
    default:
        return false;
    }
}

/// True when the value, held in a GPR, already has its upper 32 bits clear: the instruction
/// that produced it writes a 32-bit register, which zero extends on x64.
/// This depends on every path of those emitters, fallbacks included, writing a fresh 32-bit
/// register. An emitter shortcut that returns an input register as the result (x + 0, say)
/// breaks it, so take the op off this list when adding one.
inline bool GprZeroExtended32(const IR::Value& value) {
    if (value.IsImmediate() || BaselineCodegen()) {
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
    case IR::Opcode::LeastSignificantWord:  // mov r32, r32
    case IR::Opcode::MostSignificantWord:   // shr r64, 32
        return !BaselineLoadsAndImmediates();
    default:
        return false;
    }
}

class EmitX64 {
public:
    struct BlockDescriptor {
        CodePtr entrypoint;  // Entrypoint of emitted code
        size_t size;         // Length in bytes of emitted code, without the cold code in the far area
    };

    explicit EmitX64(BlockOfCode& code);
    virtual ~EmitX64();

    /// Looks up an emitted host block in the cache.
    std::optional<BlockDescriptor> GetBasicBlock(IR::LocationDescriptor descriptor) const;

    /// Empties the entire cache.
    virtual void ClearCache();

    /// Invalidates a selection of basic blocks.
    void InvalidateBasicBlocks(const tsl::robin_set<IR::LocationDescriptor>& locations);

    /// Guards the block maps when several cores share this code: shared for lookups, exclusive
    /// to compile, link, invalidate or handle a fastmem fault.
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
