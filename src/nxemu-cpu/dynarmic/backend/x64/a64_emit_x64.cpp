/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/a64_emit_x64.h"

#include <atomic>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <mcl/assert.hpp>
#include <mcl/scope_exit.hpp>
#include <mcl/stdint.hpp>
#include <mcl/type_traits/integer_of_size.hpp>

#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/abi.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/backend/x64/nzcv_util.h"
#include "dynarmic/backend/x64/perf_map.h"
#include "dynarmic/backend/x64/stack_layout.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/a64_types.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"

// TODO: Have ARM flags in host flags and not have them use up GPR registers unless necessary.
// TODO: Actually implement that proper instruction selector you've always wanted to sweetheart.

namespace Dynarmic::Backend::X64 {

using namespace Xbyak::util;

A64EmitContext::A64EmitContext(const A64::UserConfig& conf, RegAlloc& reg_alloc, IR::Block& block)
        : EmitContext(reg_alloc, block), conf(conf) {}

A64::LocationDescriptor A64EmitContext::Location() const {
    return A64::LocationDescriptor{block.Location()};
}

bool A64EmitContext::IsSingleStep() const {
    return Location().SingleStepping();
}

FP::FPCR A64EmitContext::FPCR(bool fpcr_controlled) const {
    return fpcr_controlled ? Location().FPCR() : Location().FPCR().ASIMDStandardValue();
}

A64EmitX64::A64EmitX64(BlockOfCode& code, A64::UserConfig conf, A64::Jit* jit_interface)
        : EmitX64(code), conf(conf), devirtualize_callbacks(conf.callbacks), jit_interface{jit_interface} {
    GenMemory128Accessors();
    GenFastmemFallbacks();
    GenTerminalHandlers();
    // A quarter of the cache for the cold code of the blocks (the deferred fallback paths).
    // Test variant bit 32 keeps the old layout, with the cold code after each block.
    if ((conf.test_variant & 32) == 0) {
        code.ReserveFarCode(conf.code_cache_size / 4);
    }
    code.PreludeComplete();
    ClearFastDispatchTable();

    exception_handler.SetFastmemCallback([this](u64 rip_) {
        return FastmemCallback(rip_);
    });
}

A64EmitX64::~A64EmitX64() = default;

// The emitted code of these leaves EFLAGS alone: moves only. The host flag tracking in Emit and
// PlanNZCVAtExits both use this list. vector_moves is false for test variant bit 512.
static bool IsHostFlagNeutral(const IR::Inst& inst, bool vector_moves) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::Void:
    case IR::Opcode::A64SetNZCVAtExits:
    case IR::Opcode::A64SetRegAtExits:
    case IR::Opcode::A64GetW:
    case IR::Opcode::A64GetX:
    case IR::Opcode::A64GetSP:
    case IR::Opcode::A64SetW:
    case IR::Opcode::A64SetX:
    case IR::Opcode::A64SetSP:
    case IR::Opcode::Identity:
        return true;
    // Vector register moves and zero extensions: SSE moves, lane extracts and mov r32, r32,
    // which leave EFLAGS alone. An immediate is loaded into an XMM with xorps or movaps, but
    // into a GPR with xor.
    case IR::Opcode::A64GetS:
    case IR::Opcode::A64GetD:
    case IR::Opcode::A64GetQ:
    case IR::Opcode::A64SetS:
    case IR::Opcode::A64SetD:
    case IR::Opcode::A64SetQ:
    case IR::Opcode::VectorGetElement32:
    case IR::Opcode::VectorGetElement64:
    case IR::Opcode::VectorZeroUpper:
    case IR::Opcode::ZeroExtendLongToQuad:
        return vector_moves;
    case IR::Opcode::ZeroExtendWordToLong:
        return vector_moves && !inst.GetArg(0).IsImmediate();
    default:
        return false;
    }
}

// The NZCV a SetNZCV stores is often overwritten before anything but a side exit reads
// it. Look ahead from each SetNZCV (the block is known in full): if every later reader before
// the next NZCV write is a side exit or a conditional select that finds the value still in the
// host flags, and the flags also survive to the block end when nothing overwrites NZCV first,
// the producer leaves NZCV in the host flags only and the side exits and the block end store
// it (EmitStoreNZCVFromHostFlags). An NZCV overwritten before the next exit is never stored.
// Only GetNZCVFromOp values (Add/Sub, or a test of a result) qualify, not FCMP's.
void A64EmitX64::PlanNZCVAtExits(A64EmitContext& ctx) const {
    IR::Block& block = ctx.block;
    const bool vector_moves = (conf.test_variant & 512) == 0;
    for (auto it = block.begin(); it != block.end(); ++it) {
        IR::Inst& set = *it;
        if (set.GetOpcode() != IR::Opcode::A64SetNZCV || set.GetArg(0).IsImmediate()) {
            continue;
        }
        IR::Inst* const get = set.GetArg(0).GetInst();
        if (get->GetOpcode() != IR::Opcode::GetNZCVFromOp || get->UseCount() != 1) {
            continue;
        }
        IR::Inst* const producer = get->GetArg(0).GetInst();
        const bool pseudo_of_producer = producer->GetAssociatedPseudoOperation(IR::Opcode::GetNZCVFromOp) == get
                                        && (producer->GetOpcode() == IR::Opcode::Add32 || producer->GetOpcode() == IR::Opcode::Add64
                                            || producer->GetOpcode() == IR::Opcode::Sub32 || producer->GetOpcode() == IR::Opcode::Sub64);

        // The flags must survive from where they are made (the producer, or the test that
        // GetNZCVFromOp emits) to the SetNZCV.
        bool ok = false;
        bool found_start = false;
        for (auto back = block.begin(); back != it; ++back) {
            if (&*back == (pseudo_of_producer ? producer : get)) {
                found_start = true;
                ok = true;
                continue;
            }
            if (found_start && &*back != get && !IsHostFlagNeutral(*back, vector_moves)) {
                ok = false;
            }
        }
        if (!ok) {
            continue;
        }

        // Look ahead to the next NZCV write
        bool flags_valid = true;
        bool overwritten = false;
        for (auto next = std::next(it); next != block.end() && ok; ++next) {
            const IR::Inst& inst = *next;
            const IR::Opcode op = inst.GetOpcode();
            if (op == IR::Opcode::A64SetNZCV || op == IR::Opcode::A64SetNZCVRaw || op == IR::Opcode::A64SetNZCVAtExits) {
                overwritten = true;
                break;
            }
            if (op == IR::Opcode::A64SideExit) {
                const IR::Cond cond = inst.GetArg(0).GetCond();
                // AL/NV exits test a bit (a test, or IsZero's ZF): the flags are gone at the exit
                const bool unconditional = (cond == IR::Cond::AL || cond == IR::Cond::NV) && inst.GetArg(1).IsImmediate();
                ok = flags_valid && (unconditional || (cond != IR::Cond::AL && cond != IR::Cond::NV));
                continue;
            }
            if (op == IR::Opcode::ConditionalSelect32 || op == IR::Opcode::ConditionalSelect64 || op == IR::Opcode::ConditionalSelectNZCV) {
                const IR::Cond cond = inst.GetArg(0).GetCond();
                ok = flags_valid && cond != IR::Cond::AL && cond != IR::Cond::NV;
                if (cond == IR::Cond::HI || cond == IR::Cond::LS) {
                    flags_valid = false;  // the select inverts CF for these
                }
                continue;
            }
            if (inst.ReadsFromCPSR() || inst.CausesCPUException() || op == IR::Opcode::CallHostFunction
                || op == IR::Opcode::A64DataCacheOperationRaised || op == IR::Opcode::A64InstructionCacheOperationRaised) {
                ok = false;  // reads NZCV from the guest state, or something may look at it
                continue;
            }
            if (!IsHostFlagNeutral(inst, vector_moves)) {
                flags_valid = false;
            }
        }
        if (!ok || (!overwritten && !flags_valid)) {
            continue;  // a reader would not find it, or the block end could not store it
        }

        set.Invalidate();
        set.ReplaceOpcode(IR::Opcode::A64SetNZCVAtExits);
        ctx.nzcv_at_exits.emplace_back(&set, get);
    }
}

// A block that runs past conditional branches (side exits) stores every write of a guest
// register before each exit, so a register written between several exits is stored several
// times on the hot path. A SetW/SetX followed by a later write of the same register, with only
// side exits in between that could see it, becomes A64SetRegAtExits: the hot path stores just
// the last write, and each side exit in between stores the value from wherever it is at that
// exit. The value stays live until after the last of those exits (A64SetRegAtExits uses it
// there), so its host location is still its own at every exit.
void A64EmitX64::PlanRegistersAtExits(A64EmitContext& ctx) const {
    IR::Block& block = ctx.block;
    const auto is_set = [](const IR::Inst& inst) {
        return inst.GetOpcode() == IR::Opcode::A64SetW || inst.GetOpcode() == IR::Opcode::A64SetX;
    };
    const auto is_get = [](const IR::Inst& inst) {
        return inst.GetOpcode() == IR::Opcode::A64GetW || inst.GetOpcode() == IR::Opcode::A64GetX;
    };
    for (auto it = block.begin(); it != block.end(); ++it) {
        IR::Inst& set = *it;
        if (!is_set(set)) {
            continue;
        }
        const A64::Reg reg = set.GetArg(0).GetA64RegRef();
        std::vector<const IR::Inst*> exits;
        bool ok = true;
        bool replaced = false;
        for (auto next = std::next(it); next != block.end(); ++next) {
            const IR::Inst& inst = *next;
            const IR::Opcode op = inst.GetOpcode();
            if (is_set(inst) && inst.GetArg(0).GetA64RegRef() == reg) {
                replaced = true;
                break;
            }
            if (is_get(inst) && inst.GetArg(0).GetA64RegRef() == reg) {
                ok = false;  // reads it back from the JIT state
                break;
            }
            if (op == IR::Opcode::A64SideExit) {
                exits.push_back(&inst);
                continue;
            }
            if (inst.CausesCPUException() || op == IR::Opcode::A64CallSupervisor || op == IR::Opcode::A64ExceptionRaised
                || op == IR::Opcode::CallHostFunction || op == IR::Opcode::A64DataCacheOperationRaised
                || op == IR::Opcode::A64InstructionCacheOperationRaised) {
                ok = false;  // something outside the block may look at the guest state
                break;
            }
        }
        if (!ok || !replaced || exits.empty()) {
            continue;
        }

        const IR::Value value = set.GetArg(1);
        const bool is_w = set.GetOpcode() == IR::Opcode::A64SetW;
        const EmitContext::SetAtExit pending{
            offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg),
            is_w,
            is_w && GprZeroExtended32(value),
            value.IsImmediate() ? value : IR::Value(value.GetInstRecursive()),
        };
        for (const IR::Inst* exit : exits) {
            ctx.sets_at_exits[exit].push_back(pending);
        }
        // The write moves to just after the last of its exits, as A64SetRegAtExits: its use of
        // the value keeps the value's host location until then, and it emits nothing.
        auto after_last = block.begin();
        while (&*after_last != exits.back()) {
            ++after_last;
        }
        block.PrependNewInst(std::next(after_last), IR::Opcode::A64SetRegAtExits, {pending.value});
        set.Invalidate();
    }
}

// Store the guest NZCV that the host flags hold (CF = guest C) in the layout cpsr_nzcv keeps:
// the lahf byte in bits 8-15 and OF in bit 0. Clobbers RAX, keeps the flags. mov eax, 0 (not
// xor, which would change the flags) breaks the dependency on RAX's last writer for lahf.
void A64EmitX64::EmitStoreNZCVFromHostFlags() {
    code.mov(eax, 0);
    code.lahf();
    code.seto(code.al);
    code.mov(dword[r15 + offsetof(A64JitState, cpsr_nzcv)], eax);
}

A64EmitX64::BlockDescriptor A64EmitX64::Emit(IR::Block& block) {
    if (conf.very_verbose_debugging_output) {
        std::puts(IR::DumpBlock(block).c_str());
    }
    jit_test_variant = conf.test_variant;
    if (conf.test_variant & 4) {  // differential tests: the IR of each block, to jit-ir.txt
        if (FILE* f = std::fopen("jit-ir.txt", "a")) {
            std::fprintf(f, "variant %u\n%s\n", conf.test_variant, IR::DumpBlock(block).c_str());
            std::fclose(f);
        }
    }

    code.EnableWriting();
    SCOPE_EXIT {
        code.DisableWriting();
    };

    const std::vector<HostLoc> gpr_order = [this] {
        std::vector<HostLoc> gprs{any_gpr};
        if (conf.page_table) {
            gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R14));
        }
        if (conf.fastmem_pointer) {
            gprs.erase(std::find(gprs.begin(), gprs.end(), HostLoc::R13));
        }
        return gprs;
    }();

    RegAlloc reg_alloc{code, gpr_order, any_xmm};
    A64EmitContext ctx{conf, reg_alloc, block};
    ctx.reuse_host_flags = (conf.test_variant & 8) == 0 && !conf.very_verbose_debugging_output;
    // Test variant bit 1024 stores NZCV at every flag-setting instruction (the code generation before).
    // A memory access that halts on an abort leaves the block in the middle, so the guest state must
    // hold NZCV at every memory access: no plan then.
    if (ctx.reuse_host_flags && (conf.test_variant & 1024) == 0 && !conf.check_halt_on_memory_access) {
        PlanNZCVAtExits(ctx);
    }
    // Test variant bit 2048 stores every guest register write on the hot path (the code
    // generation before). Not with the memory-abort check either, for the same reason.
    if ((conf.test_variant & 2048) == 0 && !conf.check_halt_on_memory_access) {
        PlanRegistersAtExits(ctx);
    }
    // Test variant bit 512: block ends reload NZCV and the check bit, and vector moves count as
    // changing the host flags (the code generation before)
    const bool flags_to_block_end = (conf.test_variant & 512) == 0;
    const IR::Terminal terminal = block.GetTerminal();
    ctx.check_bit_exit_on_flags = flags_to_block_end && !conf.enable_cycle_counting && boost::get<IR::Term::CheckBit>(&terminal) != nullptr;
    terminal_check_bit_in_zf = false;

    // Start emitting.
    code.align();
    const u8* const entrypoint = code.getCurr();

    ASSERT(block.GetCondition() == IR::Cond::AL);

    for (auto iter = block.begin(); iter != block.end(); ++iter) {
        IR::Inst* inst = &*iter;
        const IR::Opcode op = inst->GetOpcode();
        const bool flag_producer = op == IR::Opcode::Add32 || op == IR::Opcode::Add64 || op == IR::Opcode::Sub32 || op == IR::Opcode::Sub64
                                   || op == IR::Opcode::FPCompare32 || op == IR::Opcode::FPCompare64;
        if (flag_producer) {
            ctx.host_flags_nzcv = nullptr;  // set again by the emitter when it makes NZCV
            ctx.host_flags_fcmp = false;
        }

        // Call the relevant Emit* member function.
        switch (inst->GetOpcode()) {
#define OPCODE(name, type, ...)            \
    case IR::Opcode::name:                 \
        A64EmitX64::Emit##name(ctx, inst); \
        break;
#define A32OPC(...)
#define A64OPC(name, type, ...)               \
    case IR::Opcode::A64##name:               \
        A64EmitX64::EmitA64##name(ctx, inst); \
        break;
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC

        default:
            ASSERT_MSG(false, "Invalid opcode: {}", inst->GetOpcode());
            break;
        }

        ctx.reg_alloc.EndOfAllocScope();
        ctx.reg_alloc.EndOfInstruction();

        if (op != IR::Opcode::IsZero32 && op != IR::Opcode::IsZero64) {
            ctx.zero_flag_for = nullptr;
        }

        // Track whether the host flags still hold the guest NZCV. Only the opcodes listed
        // here keep the flags; their emitted code is moves only.
        switch (op) {
        case IR::Opcode::Add32:
        case IR::Opcode::Add64:
        case IR::Opcode::Sub32:
        case IR::Opcode::Sub64:
        case IR::Opcode::GetNZCVFromOp:
            ctx.host_flags_are_cpsr = false;  // the emitter set host_flags_nzcv
            ctx.host_flags_fcmp = false;
            break;
        case IR::Opcode::FPCompare32:
        case IR::Opcode::FPCompare64:
            ctx.host_flags_are_cpsr = false;  // the emitter set host_flags_nzcv and host_flags_fcmp
            break;
        case IR::Opcode::A64SetNZCV:
            ctx.host_flags_are_cpsr = ctx.reuse_host_flags && ctx.host_flags_nzcv && !inst->GetArg(0).IsImmediate() && inst->GetArg(0).GetInst() == ctx.host_flags_nzcv;
            break;
        case IR::Opcode::A64SetNZCVAtExits:
            ctx.host_flags_are_cpsr = true;  // EmitA64SetNZCVAtExits checked host_flags_nzcv
            break;
        case IR::Opcode::A64SideExit:
        case IR::Opcode::ConditionalSelect32:
        case IR::Opcode::ConditionalSelect64:
        case IR::Opcode::ConditionalSelectNZCV:
            if (!ctx.host_flags_are_cpsr) {
                ctx.host_flags_nzcv = nullptr;
            }
            break;
        default:
            if (!IsHostFlagNeutral(*inst, flags_to_block_end)) {
                ctx.host_flags_nzcv = nullptr;
                ctx.host_flags_are_cpsr = false;
                ctx.host_flags_fcmp = false;
            }
            break;
        }

        if (conf.very_verbose_debugging_output) {
            EmitVerboseDebuggingOutput(reg_alloc);
        }
    }

    reg_alloc.AssertNoMoreUses();

    if (ctx.nzcv_pending) {
        // The last NZCV of the block is still only in the host flags (PlanNZCVAtExits)
        ASSERT_MSG(ctx.host_flags_are_cpsr, "NZCV left in the host flags, but they changed before the block end");
        EmitStoreNZCVFromHostFlags();
        ctx.nzcv_pending = false;
    }
    if (conf.enable_cycle_counting) {
        EmitAddCycles(block.CycleCount());
    }
    // The If exit tests the host flags when they still hold the stored NZCV; the cycle count
    // update would change them, and only an If at the top tests before emitting other code.
    if (flags_to_block_end && !conf.enable_cycle_counting && ctx.host_flags_are_cpsr && boost::get<IR::Term::If>(&terminal) != nullptr) {
        terminal_flags = ctx.host_flags_fcmp ? TerminalFlags::FCompare : TerminalFlags::Guest;
    }
    EmitX64::EmitTerminal(terminal, ctx.Location().SetSingleStepping(false), ctx.IsSingleStep());
    terminal_flags.reset();
    code.int3();
    const size_t size = static_cast<size_t>(code.getCurr() - entrypoint);

    // The cold code goes to the far area, so the hot code of consecutive blocks stays dense
    const bool far_code = code.HasFarCode();
    if (far_code) {
        code.SwitchToFarCode();
    }
    for (auto& deferred_emit : ctx.deferred_emits) {
        deferred_emit();
    }
    EmitLinkStubs();
    code.int3();
    if (far_code) {
        code.SwitchToNearCode();
    }

    const A64::LocationDescriptor descriptor{block.Location()};
    const A64::LocationDescriptor end_location{block.EndLocation()};

    const auto range = boost::icl::discrete_interval<u64>::closed(descriptor.PC(), end_location.PC() - 1);
    block_ranges.AddRange(range, descriptor);

    return RegisterBlock(descriptor, entrypoint, size);
}

void A64EmitX64::ClearCache() {
    EmitX64::ClearCache();
    link_sites.clear();
    pending_link_stubs.clear();
    block_ranges.ClearCache();
    ClearFastDispatchTable();
    fastmem_patch_info.clear();
}

void A64EmitX64::InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges) {
    InvalidateBasicBlocks(block_ranges.InvalidateRanges(ranges));
}

void A64EmitX64::ClearFastDispatchTable() {
    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        for (FastDispatchTable* table : fast_dispatch_tables) {
            table->fill({});
        }
    }
}

void A64EmitX64::AddFastDispatchTable(FastDispatchTable* table) {
    table->fill({});
    fast_dispatch_tables.push_back(table);
}

void A64EmitX64::RemoveFastDispatchTable(FastDispatchTable* table) {
    std::erase(fast_dispatch_tables, table);
}

void A64EmitX64::GenTerminalHandlers() {
    // PC ends up in rbp, location_descriptor ends up in rbx
    const auto calculate_location_descriptor = [this] {
        // This calculation has to match up with A64::LocationDescriptor::UniqueHash
        // TODO: Optimization is available here based on known state of fpcr.
        code.mov(rbp, qword[r15 + offsetof(A64JitState, pc)]);
        code.mov(rcx, A64::LocationDescriptor::pc_mask);
        code.and_(rcx, rbp);
        code.mov(ebx, dword[r15 + offsetof(A64JitState, fpcr)]);
        code.and_(ebx, A64::LocationDescriptor::fpcr_mask);
        code.shl(rbx, A64::LocationDescriptor::fpcr_shift);
        code.or_(rbx, rcx);
    };

    Xbyak::Label fast_dispatch_cache_miss, rsb_cache_miss;

    code.align();
    terminal_handler_pop_rsb_hint = code.getCurr<const void*>();
    calculate_location_descriptor();
    code.mov(eax, dword[r15 + offsetof(A64JitState, rsb_ptr)]);
    code.sub(eax, 1);
    code.and_(eax, u32(A64JitState::RSBPtrMask));
    code.mov(dword[r15 + offsetof(A64JitState, rsb_ptr)], eax);
    code.cmp(rbx, qword[r15 + offsetof(A64JitState, rsb_location_descriptors) + rax * sizeof(u64)]);
    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        code.jne(rsb_cache_miss);
    } else {
        code.jne(code.GetReturnFromRunCodeAddress());
    }
    code.mov(rax, qword[r15 + offsetof(A64JitState, rsb_codeptrs) + rax * sizeof(u64)]);
    code.jmp(rax);
    PerfMapRegister(terminal_handler_pop_rsb_hint, code.getCurr(), "a64_terminal_handler_pop_rsb_hint");

    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        code.align();
        terminal_handler_fast_dispatch_hint = code.getCurr<const void*>();
        calculate_location_descriptor();
        code.L(rsb_cache_miss);
        code.mov(r12, qword[r15 + offsetof(A64JitState, core_fast_dispatch_table)]);
        code.mov(rbp, rbx);
        if (code.HasHostFeature(HostFeature::SSE42)) {
            code.crc32(rbp, r12);
        }
        code.and_(ebp, fast_dispatch_table_mask);
        code.lea(rbp, ptr[r12 + rbp]);
        code.cmp(rbx, qword[rbp + offsetof(FastDispatchEntry, location_descriptor)]);
        code.jne(fast_dispatch_cache_miss);
        code.jmp(ptr[rbp + offsetof(FastDispatchEntry, code_ptr)]);
        code.L(fast_dispatch_cache_miss);
        code.mov(qword[rbp + offsetof(FastDispatchEntry, location_descriptor)], rbx);
        code.LookupBlock();
        code.mov(ptr[rbp + offsetof(FastDispatchEntry, code_ptr)], rax);
        code.jmp(rax);
        PerfMapRegister(terminal_handler_fast_dispatch_hint, code.getCurr(), "a64_terminal_handler_fast_dispatch_hint");

        code.align();
        fast_dispatch_table_lookup = code.getCurr<FastDispatchEntry& (*)(u64, FastDispatchEntry*)>();
        if (code.HasHostFeature(HostFeature::SSE42)) {
            code.crc32(code.ABI_PARAM1, code.ABI_PARAM2);
        }
        code.and_(code.ABI_PARAM1.cvt32(), fast_dispatch_table_mask);
        code.lea(code.ABI_RETURN, code.ptr[code.ABI_PARAM2 + code.ABI_PARAM1]);
        code.ret();
        PerfMapRegister(fast_dispatch_table_lookup, code.getCurr(), "a64_fast_dispatch_table_lookup");
    }
}

void A64EmitX64::EmitPushRSB(EmitContext& ctx, IR::Inst* inst) {
    if (!conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
        return;
    }

    EmitX64::EmitPushRSB(ctx, inst);
}

void A64EmitX64::EmitA64SetCheckBit(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    if (!inst->GetArg(0).IsImmediate() && ctx.zero_flag_for == inst->GetArg(0).GetInst()) {
        // ZF holds the bit (EmitIsZero fused): the block's CheckBit exit jumps on it.
        terminal_check_bit_in_zf = true;
        return;
    }
    const Xbyak::Reg8 to_store = ctx.reg_alloc.UseGpr(args[0]).cvt8();
    code.mov(code.byte[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, check_bit)], to_store);
}

void A64EmitX64::EmitA64GetCFlag(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr().cvt32();
    code.mov(result, dword[r15 + offsetof(A64JitState, cpsr_nzcv)]);
    code.shr(result, NZCV::x64_c_flag_bit);
    code.and_(result, 1);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetNZCVRaw(A64EmitContext& ctx, IR::Inst* inst) {
    ASSERT_MSG(!ctx.nzcv_pending, "GetNZCVRaw while NZCV is only in the host flags");
    const Xbyak::Reg32 nzcv_raw = ctx.reg_alloc.ScratchGpr().cvt32();

    code.mov(nzcv_raw, dword[r15 + offsetof(A64JitState, cpsr_nzcv)]);

    if (code.HasHostFeature(HostFeature::FastBMI2)) {
        const Xbyak::Reg32 tmp = ctx.reg_alloc.ScratchGpr().cvt32();
        code.mov(tmp, NZCV::x64_mask);
        code.pext(nzcv_raw, nzcv_raw, tmp);
        code.shl(nzcv_raw, 28);
    } else {
        code.and_(nzcv_raw, NZCV::x64_mask);
        code.imul(nzcv_raw, nzcv_raw, NZCV::from_x64_multiplier);
        code.and_(nzcv_raw, NZCV::arm_mask);
    }

    ctx.reg_alloc.DefineValue(inst, nzcv_raw);
}

void A64EmitX64::EmitA64SetNZCVRaw(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg32 nzcv_raw = ctx.reg_alloc.UseScratchGpr(args[0]).cvt32();

    code.shr(nzcv_raw, 28);
    if (code.HasHostFeature(HostFeature::FastBMI2)) {
        const Xbyak::Reg32 tmp = ctx.reg_alloc.ScratchGpr().cvt32();
        code.mov(tmp, NZCV::x64_mask);
        code.pdep(nzcv_raw, nzcv_raw, tmp);
    } else {
        code.imul(nzcv_raw, nzcv_raw, NZCV::to_x64_multiplier);
        code.and_(nzcv_raw, NZCV::x64_mask);
    }
    code.mov(dword[r15 + offsetof(A64JitState, cpsr_nzcv)], nzcv_raw);
    ctx.nzcv_pending = false;
}

void A64EmitX64::EmitA64SetNZCV(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg32 to_store = ctx.reg_alloc.UseScratchGpr(args[0]).cvt32();
    code.mov(dword[r15 + offsetof(A64JitState, cpsr_nzcv)], to_store);
    ctx.nzcv_pending = false;
}

// A SetNZCV that PlanNZCVAtExits moved to the side exits and the block end. Nothing to
// emit; from here the host flags are the guest NZCV until they are stored.
void A64EmitX64::EmitA64SetNZCVAtExits(A64EmitContext& ctx, IR::Inst* inst) {
    const IR::Inst* get = nullptr;
    for (const auto& [set, value] : ctx.nzcv_at_exits) {
        if (set == inst) {
            get = value;
        }
    }
    ASSERT_MSG(get && ctx.host_flags_nzcv == get && !ctx.host_flags_fcmp, "NZCV planned for the host flags is not in them");
    ctx.nzcv_pending = true;
}

// A register write that PlanRegistersAtExits moved to the side exits, placed after the last of
// them. Nothing to emit; reading the argument ends the value's use here.
void A64EmitX64::EmitA64SetRegAtExits(A64EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.GetArgumentInfo(inst);
}

void A64EmitX64::EmitA64GetW(A64EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr().cvt32();

    code.mov(result, dword[r15 + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)]);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetX(A64EmitContext& ctx, IR::Inst* inst) {
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr();

    code.mov(result, qword[r15 + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)]);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetS(A64EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = qword[r15 + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
    code.movd(result, addr);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetD(A64EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = qword[r15 + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
    code.movq(result, addr);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetQ(A64EmitContext& ctx, IR::Inst* inst) {
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = xword[r15 + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm();
    code.movaps(result, addr);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetSP(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr();
    code.mov(result, qword[r15 + offsetof(A64JitState, sp)]);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetFPCR(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr().cvt32();
    code.mov(result, dword[r15 + offsetof(A64JitState, fpcr)]);
    ctx.reg_alloc.DefineValue(inst, result);
}

static u32 GetFPSRImpl(A64JitState* jit_state) {
    return jit_state->GetFpsr();
}

void A64EmitX64::EmitA64GetFPSR(A64EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(inst);
    code.mov(code.ABI_PARAM1, code.r15);
    code.stmxcsr(code.dword[code.r15 + offsetof(A64JitState, guest_MXCSR)]);
    code.CallFunction(GetFPSRImpl);
}

void A64EmitX64::EmitA64SetW(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    const auto addr = qword[r15 + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)];
    if (args[1].FitsInImmediateS32()) {
        code.mov(addr, args[1].GetImmediateS32());
    } else if (args[1].IsImmediate() && !BaselineCodegen()) {
        // A 32-bit move clears the upper half
        const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr();
        code.mov(tmp.cvt32(), args[1].GetImmediateU32());
        code.mov(addr, tmp);
    } else {
        // TODO: xmm variant
        if (GprZeroExtended32(inst->GetArg(1))) {
            code.mov(addr, ctx.reg_alloc.UseGpr(args[1]));
            return;
        }
        const Xbyak::Reg64 to_store = ctx.reg_alloc.UseScratchGpr(args[1]);
        code.mov(to_store.cvt32(), to_store.cvt32());
        code.mov(addr, to_store);
    }
}

void A64EmitX64::EmitA64SetX(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    const auto addr = qword[r15 + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)];
    if (args[1].FitsInImmediateS32()) {
        code.mov(addr, args[1].GetImmediateS32());
    } else if (args[1].IsInXmm()) {
        const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(args[1]);
        code.movq(addr, to_store);
    } else {
        const Xbyak::Reg64 to_store = ctx.reg_alloc.UseGpr(args[1]);
        code.mov(addr, to_store);
    }
}

// The 128-bit value is zero above bit 31: UpperLanesClear, or a word zero-extended twice (the
// value an LDR S or LDP S writes).
static bool ZeroAbove32(const IR::Value& value) {
    if (BaselineLoadsAndImmediates()) {
        return false;
    }
    if (UpperLanesClear(value, 32)) {
        return true;
    }
    if (value.IsImmediate()) {
        return false;
    }
    const IR::Inst* inst = value.GetInstRecursive();
    if (inst->GetOpcode() != IR::Opcode::ZeroExtendLongToQuad || inst->GetArg(0).IsImmediate()) {
        return false;
    }
    return inst->GetArg(0).GetInstRecursive()->GetOpcode() == IR::Opcode::ZeroExtendWordToLong;
}

// The 128-bit value is zero above bit 63: as above, UpperLanesClear, or a VectorZeroUpper (the
// translator wraps every SetD value in one).
static bool ZeroAbove64(const IR::Value& value) {
    if (BaselineLoadsAndImmediates()) {
        return false;
    }
    if (ZeroAbove32(value) || UpperLanesClear(value, 64)) {
        return true;
    }
    return !value.IsImmediate() && value.GetInstRecursive()->GetOpcode() == IR::Opcode::VectorZeroUpper;
}

void A64EmitX64::EmitA64SetS(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = xword[r15 + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    if (ZeroAbove32(inst->GetArg(1))) {
        // Already zero above the written word: a plain store.
        const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(args[1]);
        code.movaps(addr, to_store);
        return;
    }
    const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(args[1]);
    const Xbyak::Xmm tmp = ctx.reg_alloc.ScratchXmm();
    // TODO: Optimize
    code.pxor(tmp, tmp);
    code.movss(tmp, to_store);
    code.movaps(addr, tmp);
}

void A64EmitX64::EmitA64SetD(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = xword[r15 + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    if (ZeroAbove64(inst->GetArg(1))) {
        // Already zero above the written doubleword: a plain store.
        const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(args[1]);
        code.movaps(addr, to_store);
        return;
    }
    const Xbyak::Xmm to_store = ctx.reg_alloc.UseScratchXmm(args[1]);
    code.movq(to_store, to_store);  // TODO: Remove when able
    code.movaps(addr, to_store);
}

void A64EmitX64::EmitA64SetQ(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = xword[r15 + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

    const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(args[1]);
    code.movaps(addr, to_store);
}

void A64EmitX64::EmitA64SetSP(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto addr = qword[r15 + offsetof(A64JitState, sp)];
    if (args[0].FitsInImmediateS32()) {
        code.mov(addr, args[0].GetImmediateS32());
    } else if (args[0].IsInXmm()) {
        const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(args[0]);
        code.movq(addr, to_store);
    } else {
        const Xbyak::Reg64 to_store = ctx.reg_alloc.UseGpr(args[0]);
        code.mov(addr, to_store);
    }
}

static void SetFPCRImpl(A64JitState* jit_state, u32 value) {
    jit_state->SetFpcr(value);
}

void A64EmitX64::EmitA64SetFPCR(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(nullptr, {}, args[0]);
    code.mov(code.ABI_PARAM1, code.r15);
    // Save the exception flags raised so far: SetFpcr rebuilds guest_MXCSR keeping only its flag
    // bits, and the ldmxcsr below would otherwise drop the ones still in the host MXCSR (FPSR was
    // lost across an FPCR write).
    code.stmxcsr(code.dword[code.r15 + offsetof(A64JitState, guest_MXCSR)]);
    code.CallFunction(SetFPCRImpl);
    code.ldmxcsr(code.dword[code.r15 + offsetof(A64JitState, guest_MXCSR)]);
}

static void SetFPSRImpl(A64JitState* jit_state, u32 value) {
    jit_state->SetFpsr(value);
}

void A64EmitX64::EmitA64SetFPSR(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(nullptr, {}, args[0]);
    code.mov(code.ABI_PARAM1, code.r15);
    code.CallFunction(SetFPSRImpl);
    code.ldmxcsr(code.dword[code.r15 + offsetof(A64JitState, guest_MXCSR)]);
}

// Jumps to `target` when `cond` holds, on the host flags of the ucomis that made NZCV:
// unordered ZF PF CF = 1 1 1, equal ZF, less CF, greater none. Guest NZCV: unordered 0011,
// equal 0110, less 1000, greater 0010. Some conditions need PF to tell unordered apart. `type`
// is the jump type to `target`.
static void JumpOnFCompareFlags(BlockOfCode& code, IR::Cond cond, Xbyak::Label& target, Xbyak::CodeGenerator::LabelType type) {
    Xbyak::Label skip;
    switch (cond) {
    case IR::Cond::EQ:  // equal
        code.jp(skip);
        code.jz(target, type);
        break;
    case IR::Cond::NE:  // not equal
        code.jnz(target, type);
        code.jp(target, type);
        break;
    case IR::Cond::CS:  // not less
    case IR::Cond::PL:
        code.jae(target, type);
        code.jp(target, type);
        break;
    case IR::Cond::CC:  // less
    case IR::Cond::MI:
        code.jp(skip);
        code.jb(target, type);
        break;
    case IR::Cond::VS: code.jp(target, type); break;   // unordered
    case IR::Cond::VC: code.jnp(target, type); break;  // ordered
    case IR::Cond::HI:  // greater or unordered
        code.ja(target, type);
        code.jp(target, type);
        break;
    case IR::Cond::LS:  // less or equal
        code.jp(skip);
        code.jbe(target, type);
        break;
    case IR::Cond::GE: code.jae(target, type); break;  // greater or equal
    case IR::Cond::LT: code.jb(target, type); break;   // less or unordered
    case IR::Cond::GT: code.ja(target, type); break;   // greater
    case IR::Cond::LE: code.jbe(target, type); break;  // less, equal or unordered
    default: ASSERT_MSG(false, "Unknown cond {}", static_cast<size_t>(cond)); break;
    }
    code.L(skip);
}

// Jumps to `target` when `cond` holds, on host flags that hold the guest NZCV (SF ZF CF OF),
// with CF already inverted for HI and LS. `type` is the jump type to `target`.
static void JumpOnNZCVFlags(BlockOfCode& code, IR::Cond cond, Xbyak::Label& target, Xbyak::CodeGenerator::LabelType type) {
    switch (cond) {
    case IR::Cond::EQ: code.jz(target, type); break;
    case IR::Cond::NE: code.jnz(target, type); break;
    case IR::Cond::CS: code.jc(target, type); break;
    case IR::Cond::CC: code.jnc(target, type); break;
    case IR::Cond::MI: code.js(target, type); break;
    case IR::Cond::PL: code.jns(target, type); break;
    case IR::Cond::VS: code.jo(target, type); break;
    case IR::Cond::VC: code.jno(target, type); break;
    case IR::Cond::HI: code.ja(target, type); break;
    case IR::Cond::LS: code.jna(target, type); break;
    case IR::Cond::GE: code.jge(target, type); break;
    case IR::Cond::LT: code.jl(target, type); break;
    case IR::Cond::GT: code.jg(target, type); break;
    case IR::Cond::LE: code.jle(target, type); break;
    default: ASSERT_MSG(false, "Unknown cond {}", static_cast<size_t>(cond)); break;
    }
}

void A64EmitX64::EmitA64SideExit(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const IR::Cond cond = args[0].GetImmediateCond();
    const IR::LocationDescriptor next = ctx.Location().SetPC(args[2].GetImmediateU64());
    const size_t cycles = static_cast<size_t>(args[3].GetImmediateU64());
    const IR::LocationDescriptor location = ctx.Location().SetSingleStepping(false);
    const bool is_single_step = ctx.IsSingleStep();

    SharedLabel exit = GenSharedLabel();
    bool exit_cmc = false;  // the exit's jump runs with CF inverted (HI, LS)
    if (cond == IR::Cond::AL || cond == IR::Cond::NV) {
        const bool exit_when_set = cond == IR::Cond::AL;
        ASSERT_MSG(!ctx.nzcv_pending || args[1].IsImmediate(), "a bit test exit while NZCV is only in the host flags");
        if (!inst->GetArg(1).IsImmediate() && ctx.zero_flag_for == inst->GetArg(1).GetInst()) {
            // ZF holds the bit
            if (exit_when_set) {
                code.jz(*exit, code.T_NEAR);
            } else {
                code.jnz(*exit, code.T_NEAR);
            }
        } else if (args[1].IsImmediate()) {
            if (args[1].GetImmediateU1() != exit_when_set) {
                return;  // never exits
            }
            code.jmp(*exit, code.T_NEAR);
        } else {
            const Xbyak::Reg8 bit = ctx.reg_alloc.UseGpr(args[1]).cvt8();
            code.test(bit, bit);
            if (exit_when_set) {
                code.jnz(*exit, code.T_NEAR);
            } else {
                code.jz(*exit, code.T_NEAR);
            }
        }
    } else if (ctx.host_flags_are_cpsr && ctx.host_flags_fcmp) {
        JumpOnFCompareFlags(code, cond, *exit, code.T_NEAR);
    } else {
        if (ctx.host_flags_are_cpsr) {
            // The host flags already hold the guest NZCV. Only HI and LS need CF inverted.
            if (cond == IR::Cond::HI || cond == IR::Cond::LS) {
                code.cmc();
                exit_cmc = true;
                if (!ctx.nzcv_pending) {
                    ctx.host_flags_are_cpsr = false;
                }
            }
        } else {
            ASSERT_MSG(!ctx.nzcv_pending, "a side exit reloads NZCV that is still only in the host flags");
            ctx.reg_alloc.ScratchGpr(HostLoc::RAX);
            code.mov(eax, dword[r15 + offsetof(A64JitState, cpsr_nzcv)]);
            code.LoadRequiredFlagsForCondFromRax(cond);
        }
        JumpOnNZCVFlags(code, cond, *exit, code.T_NEAR);
        if (exit_cmc && ctx.nzcv_pending) {
            code.cmc();  // the host flags stay the guest NZCV for the exits after this one
        }
    }

    // The guest registers are already stored, so the exit only has to count its cycles
    // and link to the target block, and store NZCV if it is still only in the host
    // flags (PlanNZCVAtExits); nothing is live once the block is left, so RAX is free.
    const bool store_nzcv = ctx.nzcv_pending;

    // Register writes only the side exits store (PlanRegistersAtExits), from where each value
    // is now. Values in registers are stored first, with moves (the flags may still hold NZCV);
    // spilled values and 64-bit immediates go through RAX once NZCV is stored.
    struct ExitStore {
        size_t offset;
        bool is_w;
        bool zero_extended;
        bool immediate;
        u64 imm;
        HostLoc loc;
        std::optional<Xbyak::Address> spill;
        bool FitsMove() const {  // a mov of a sign-extended 32-bit immediate stores it
            const u64 stored = is_w ? (imm & 0xFFFFFFFF) : imm;
            return static_cast<u64>(static_cast<s64>(static_cast<s32>(stored))) == stored;
        }
    };
    std::vector<ExitStore> exit_stores;
    if (const auto found = ctx.sets_at_exits.find(inst); found != ctx.sets_at_exits.end()) {
        for (const auto& pending : found->second) {
            ExitStore store{pending.offset, pending.is_w, pending.zero_extended, pending.value.IsImmediate(), 0, HostLoc::RAX, std::nullopt};
            if (store.immediate) {
                store.imm = pending.is_w ? pending.value.GetImmediateAsU64() & 0xFFFFFFFF : pending.value.GetImmediateAsU64();
            } else {
                const auto loc = ctx.reg_alloc.CurrentLocation(pending.value.GetInst());
                ASSERT_MSG(loc, "a register write planned for the side exits is no longer live");
                store.loc = *loc;
                if (HostLocIsSpill(store.loc)) {
                    store.spill = ctx.reg_alloc.SpillAddress(store.loc);
                }
            }
            exit_stores.push_back(store);
        }
    }

    ctx.deferred_emits.emplace_back([=, this] {
        code.L(*exit);
        for (const ExitStore& store : exit_stores) {
            const auto addr = qword[r15 + store.offset];
            if (store.immediate) {
                if (store.FitsMove()) {
                    code.mov(addr, static_cast<s32>(store.imm));
                }
            } else if (HostLocIsGPR(store.loc)) {
                const Xbyak::Reg64 reg = HostLocToReg64(store.loc);
                if (store.is_w && !store.zero_extended) {
                    code.mov(dword[r15 + store.offset], reg.cvt32());
                    code.mov(dword[r15 + store.offset + 4], 0);
                } else {
                    code.mov(addr, reg);
                }
            } else if (HostLocIsXMM(store.loc)) {
                const Xbyak::Xmm xmm = HostLocToXmm(store.loc);
                if (store.is_w) {
                    code.movd(dword[r15 + store.offset], xmm);
                    code.mov(dword[r15 + store.offset + 4], 0);
                } else {
                    code.movq(addr, xmm);
                }
            }
        }
        if (store_nzcv) {
            if (exit_cmc) {
                code.cmc();
            }
            EmitStoreNZCVFromHostFlags();
        }
        for (const ExitStore& store : exit_stores) {
            const auto addr = qword[r15 + store.offset];
            if (store.immediate) {
                if (!store.FitsMove()) {
                    if (store.is_w) {
                        code.mov(eax, static_cast<u32>(store.imm));
                    } else {
                        code.mov(rax, store.imm);
                    }
                    code.mov(addr, rax);
                }
            } else if (HostLocIsSpill(store.loc)) {
                if (store.is_w) {
                    code.mov(eax, dword[store.spill->getRegExp()]);
                } else {
                    code.mov(rax, qword[store.spill->getRegExp()]);
                }
                code.mov(addr, rax);
            }
        }
        if (conf.enable_cycle_counting) {
            EmitAddCycles(cycles);
        }
        EmitTerminal(IR::Term::LinkBlock{next}, location, is_single_step);
    });
}

void A64EmitX64::EmitA64SetPC(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const auto addr = qword[r15 + offsetof(A64JitState, pc)];
    if (args[0].FitsInImmediateS32()) {
        code.mov(addr, args[0].GetImmediateS32());
    } else if (args[0].IsInXmm()) {
        const Xbyak::Xmm to_store = ctx.reg_alloc.UseXmm(args[0]);
        code.movq(addr, to_store);
    } else {
        const Xbyak::Reg64 to_store = ctx.reg_alloc.UseGpr(args[0]);
        code.mov(addr, to_store);
    }
}

void A64EmitX64::EmitA64CallSupervisor(A64EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[0].IsImmediate());
    const u32 imm = args[0].GetImmediateU32();
    CoreCallback<&A64::UserCallbacks::CallSVC>().EmitCall(code, [&](RegList param) {
        code.mov(param[0], imm);
    });
    // The kernel would have to execute ERET to get here, which would clear exclusive state.
    code.mov(code.byte[r15 + offsetof(A64JitState, exclusive_state)], u8(0));
}

void A64EmitX64::EmitA64ExceptionRaised(A64EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ASSERT(args[0].IsImmediate() && args[1].IsImmediate());
    const u64 pc = args[0].GetImmediateU64();
    const u64 exception = args[1].GetImmediateU64();
    CoreCallback<&A64::UserCallbacks::ExceptionRaised>().EmitCall(code, [&](RegList param) {
        code.mov(param[0], pc);
        code.mov(param[1], exception);
    });
}

void A64EmitX64::EmitA64DataCacheOperationRaised(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(nullptr, {}, args[1], args[2]);
    CoreCallback<&A64::UserCallbacks::DataCacheOperationRaised>().EmitCall(code);
}

void A64EmitX64::EmitA64InstructionCacheOperationRaised(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    ctx.reg_alloc.HostCall(nullptr, {}, args[0], args[1]);
    CoreCallback<&A64::UserCallbacks::InstructionCacheOperationRaised>().EmitCall(code);
}

void A64EmitX64::EmitA64DataSynchronizationBarrier(A64EmitContext&, IR::Inst*) {
    code.mfence();
    code.lfence();
}

void A64EmitX64::EmitA64DataMemoryBarrier(A64EmitContext&, IR::Inst*) {
    code.mfence();
}

void A64EmitX64::EmitA64InstructionSynchronizationBarrier(A64EmitContext& ctx, IR::Inst*) {
    if (!conf.hook_isb) {
        return;
    }

    ctx.reg_alloc.HostCall(nullptr);
    CoreCallback<&A64::UserCallbacks::InstructionSynchronizationBarrierRaised>().EmitCall(code);
}

void A64EmitX64::EmitA64GetCNTFRQ(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr().cvt32();
    code.mov(result, conf.cntfrq_el0);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetCNTPCT(A64EmitContext& ctx, IR::Inst* inst) {
    ctx.reg_alloc.HostCall(inst);
    if (!conf.wall_clock_cntpct) {
        code.UpdateTicks();
    }
    CoreCallback<&A64::UserCallbacks::GetCNTPCT>().EmitCall(code);
}

void A64EmitX64::EmitA64GetCTR(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr().cvt32();
    code.mov(result, conf.ctr_el0);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetDCZID(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg32 result = ctx.reg_alloc.ScratchGpr().cvt32();
    code.mov(result, conf.dczid_el0);
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetTPIDR(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr();
    if (conf.tpidr_el0) {
        code.mov(result, qword[r15 + offsetof(A64JitState, core_tpidr_el0_ptr)]);
        code.mov(result, qword[result]);
    } else {
        code.xor_(result.cvt32(), result.cvt32());
    }
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64GetTPIDRRO(A64EmitContext& ctx, IR::Inst* inst) {
    const Xbyak::Reg64 result = ctx.reg_alloc.ScratchGpr();
    if (conf.tpidrro_el0) {
        code.mov(result, qword[r15 + offsetof(A64JitState, core_tpidrro_el0_ptr)]);
        code.mov(result, qword[result]);
    } else {
        code.xor_(result.cvt32(), result.cvt32());
    }
    ctx.reg_alloc.DefineValue(inst, result);
}

void A64EmitX64::EmitA64SetTPIDR(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg64 value = ctx.reg_alloc.UseGpr(args[0]);
    const Xbyak::Reg64 addr = ctx.reg_alloc.ScratchGpr();
    if (conf.tpidr_el0) {
        code.mov(addr, qword[r15 + offsetof(A64JitState, core_tpidr_el0_ptr)]);
        code.mov(qword[addr], value);
    }
}

std::string A64EmitX64::LocationDescriptorToFriendlyName(const IR::LocationDescriptor& ir_descriptor) const {
    const A64::LocationDescriptor descriptor{ir_descriptor};
    return fmt::format("a64_{:016X}_fpcr{:08X}",
                       descriptor.PC(),
                       descriptor.FPCR().Value());
}

void A64EmitX64::EmitTerminalImpl(IR::Term::Interpret terminal, IR::LocationDescriptor, bool) {
    code.SwitchMxcsrOnExit();
    CoreCallback<&A64::UserCallbacks::InterpreterFallback>().EmitCall(code, [&](RegList param) {
        code.mov(param[0], A64::LocationDescriptor{terminal.next}.PC());
        code.mov(qword[r15 + offsetof(A64JitState, pc)], param[0]);
        code.mov(param[1].cvt32(), terminal.num_instructions);
    });
    code.ReturnFromRunCode(true);  // TODO: Check cycles
}

void A64EmitX64::EmitTerminalImpl(IR::Term::ReturnToDispatch, IR::LocationDescriptor, bool) {
    code.ReturnFromRunCode();
}

void A64EmitX64::EmitTerminalImpl(IR::Term::LinkBlock terminal, IR::LocationDescriptor, bool is_single_step) {
    if (!conf.HasOptimization(OptimizationFlag::BlockLinking) || is_single_step) {
        code.mov(rax, A64::LocationDescriptor{terminal.next}.PC());
        code.mov(qword[r15 + offsetof(A64JitState, pc)], rax);
        code.ReturnFromRunCode();
        return;
    }

    if (conf.enable_cycle_counting) {
        code.cmp(qword[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, cycles_remaining)], 0);

        patch_information[terminal.next].jg.push_back(code.getCurr());
        if (const auto next_bb = GetBasicBlock(terminal.next)) {
            EmitPatchJg(terminal.next, next_bb->entrypoint);
        } else {
            EmitPatchJg(terminal.next);
        }
    } else {
        code.cmp(dword[r15 + offsetof(A64JitState, halt_reason)], 0);

        patch_information[terminal.next].jz.push_back(code.getCurr());
        if (const auto next_bb = GetBasicBlock(terminal.next)) {
            EmitPatchJz(terminal.next, next_bb->entrypoint);
        } else {
            EmitPatchJz(terminal.next);
        }
    }

    code.mov(rax, A64::LocationDescriptor{terminal.next}.PC());
    code.mov(qword[r15 + offsetof(A64JitState, pc)], rax);
    code.ForceReturnFromRunCode();
}

void A64EmitX64::EmitTerminalImpl(IR::Term::LinkBlockFast terminal, IR::LocationDescriptor, bool is_single_step) {
    if (!conf.HasOptimization(OptimizationFlag::BlockLinking) || is_single_step) {
        code.mov(rax, A64::LocationDescriptor{terminal.next}.PC());
        code.mov(qword[r15 + offsetof(A64JitState, pc)], rax);
        code.ReturnFromRunCode();
        return;
    }

    patch_information[terminal.next].jmp.push_back(code.getCurr());
    if (auto next_bb = GetBasicBlock(terminal.next)) {
        EmitPatchJmp(terminal.next, next_bb->entrypoint);
    } else {
        EmitPatchJmp(terminal.next);
    }
}

void A64EmitX64::EmitTerminalImpl(IR::Term::PopRSBHint, IR::LocationDescriptor, bool is_single_step) {
    if (!conf.HasOptimization(OptimizationFlag::ReturnStackBuffer) || is_single_step) {
        code.ReturnFromRunCode();
        return;
    }

    code.jmp(terminal_handler_pop_rsb_hint);
}

void A64EmitX64::EmitTerminalImpl(IR::Term::FastDispatchHint, IR::LocationDescriptor, bool is_single_step) {
    if (!conf.HasOptimization(OptimizationFlag::FastDispatch) || is_single_step) {
        code.ReturnFromRunCode();
        return;
    }

    code.jmp(terminal_handler_fast_dispatch_hint);
}

void A64EmitX64::EmitTerminalImpl(IR::Term::If terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    switch (terminal.if_) {
    case IR::Cond::AL:
    case IR::Cond::NV:
        EmitTerminal(terminal.then_, initial_location, is_single_step);
        break;
    default:
        Xbyak::Label pass;
        if (terminal_flags == TerminalFlags::FCompare) {
            // The host flags are still those of the FCMP that made the stored NZCV: no reload
            JumpOnFCompareFlags(code, terminal.if_, pass, code.T_AUTO);
            terminal_flags.reset();
        } else if (terminal_flags == TerminalFlags::Guest) {
            // The host flags still hold the stored NZCV: no reload
            if (terminal.if_ == IR::Cond::HI || terminal.if_ == IR::Cond::LS) {
                code.cmc();  // x64's carry is the inverse of Arm's for these
            }
            JumpOnNZCVFlags(code, terminal.if_, pass, code.T_AUTO);
            terminal_flags.reset();
        } else {
            pass = EmitCond(terminal.if_);
        }
        EmitTerminal(terminal.else_, initial_location, is_single_step);
        code.L(pass);
        EmitTerminal(terminal.then_, initial_location, is_single_step);
        break;
    }
}

void A64EmitX64::EmitTerminalImpl(IR::Term::CheckBit terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    Xbyak::Label fail;
    if (std::exchange(terminal_check_bit_in_zf, false)) {
        code.jnz(fail);  // ZF = the bit
    } else {
        code.cmp(code.byte[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, check_bit)], u8(0));
        code.jz(fail);
    }
    EmitTerminal(terminal.then_, initial_location, is_single_step);
    code.L(fail);
    EmitTerminal(terminal.else_, initial_location, is_single_step);
}

void A64EmitX64::EmitTerminalImpl(IR::Term::CheckHalt terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    code.cmp(dword[r15 + offsetof(A64JitState, halt_reason)], 0);
    code.jne(code.GetForceReturnFromRunCodeAddress());
    EmitTerminal(terminal.else_, initial_location, is_single_step);
}

namespace {
// Bytes of padding before an instruction of opcode_size bytes so that the field after the opcode
// is aligned on `alignment`.
size_t LinkPadding(const void* site, size_t opcode_size, size_t alignment) {
    const uintptr_t field = reinterpret_cast<uintptr_t>(site) + opcode_size;
    return (alignment - field % alignment) % alignment;
}

void StoreRel32(u8* rel32, CodePtr target) {
    const s64 displacement = reinterpret_cast<const u8*>(target) - (rel32 + 4);
    ASSERT(displacement == static_cast<s32>(displacement));
    ASSERT(reinterpret_cast<uintptr_t>(rel32) % 4 == 0);
    std::atomic_ref<u32>{*reinterpret_cast<u32*>(rel32)}.store(static_cast<u32>(static_cast<s32>(displacement)), std::memory_order_release);
}
}  // namespace

void A64EmitX64::EmitLinkSite(u8 condition_opcode, const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    const CodePtr site = code.getCurr();
    const size_t opcode_size = condition_opcode ? 2 : 1;
    code.nop(LinkPadding(site, opcode_size, 4));
    if (condition_opcode) {
        code.db(0x0F);
        code.db(condition_opcode);
    } else {
        code.db(0xE9);
    }
    u8* const rel32 = code.getCurr<u8*>();
    code.dd(0);
    if (target_code_ptr) {
        StoreRel32(rel32, target_code_ptr);
    }
    pending_link_stubs.push_back({site, rel32, A64::LocationDescriptor{target_desc}.PC(), target_code_ptr == nullptr});
}

void A64EmitX64::EmitLinkStubs() {
    for (const PendingLinkStub& pending : pending_link_stubs) {
        const CodePtr stub = code.getCurr();
        code.mov(rax, pending.target_pc);
        code.mov(qword[r15 + offsetof(A64JitState, pc)], rax);
        code.jmp(code.GetReturnFromRunCodeAddress(), code.T_NEAR);
        link_sites[pending.site] = LinkSite{pending.rel32, stub};
        if (pending.unlinked) {
            StoreRel32(pending.rel32, stub);
        }
    }
    pending_link_stubs.clear();
}

void A64EmitX64::EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    if (!OldLinkPatching()) {
        EmitLinkSite(0x8F, target_desc, target_code_ptr);
        return;
    }
    const CodePtr patch_location = code.getCurr();
    if (target_code_ptr) {
        code.jg(target_code_ptr);
    } else {
        code.mov(rax, A64::LocationDescriptor{target_desc}.PC());
        code.mov(qword[r15 + offsetof(A64JitState, pc)], rax);
        code.jg(code.GetReturnFromRunCodeAddress());
    }
    code.EnsurePatchLocationSize(patch_location, 23);
}

void A64EmitX64::EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    if (!OldLinkPatching()) {
        EmitLinkSite(0x84, target_desc, target_code_ptr);
        return;
    }
    const CodePtr patch_location = code.getCurr();
    if (target_code_ptr) {
        code.jz(target_code_ptr);
    } else {
        code.mov(rax, A64::LocationDescriptor{target_desc}.PC());
        code.mov(qword[r15 + offsetof(A64JitState, pc)], rax);
        code.jz(code.GetReturnFromRunCodeAddress());
    }
    code.EnsurePatchLocationSize(patch_location, 23);
}

void A64EmitX64::EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    if (!OldLinkPatching()) {
        EmitLinkSite(0, target_desc, target_code_ptr);
        return;
    }
    const CodePtr patch_location = code.getCurr();
    if (target_code_ptr) {
        code.jmp(target_code_ptr);
    } else {
        code.mov(rax, A64::LocationDescriptor{target_desc}.PC());
        code.mov(qword[r15 + offsetof(A64JitState, pc)], rax);
        code.jmp(code.GetReturnFromRunCodeAddress());
    }
    code.EnsurePatchLocationSize(patch_location, 22);
}

void A64EmitX64::EmitPatchMovRcx(CodePtr target_code_ptr) {
    if (!target_code_ptr) {
        target_code_ptr = code.GetReturnFromRunCodeAddress();
    }
    if (!OldLinkPatching()) {
        // mov rcx, imm64 in its 10-byte form, imm64 aligned on 8
        code.nop(LinkPadding(code.getCurr(), 2, 8));
        code.db(0x48);
        code.db(0xB9);
        code.dq(reinterpret_cast<u64>(target_code_ptr));
        return;
    }
    const CodePtr patch_location = code.getCurr();
    code.mov(code.rcx, reinterpret_cast<u64>(target_code_ptr));
    code.EnsurePatchLocationSize(patch_location, 10);
}

void A64EmitX64::Patch(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    if (OldLinkPatching()) {
        EmitX64::Patch(target_desc, target_code_ptr);
        return;
    }
    const tsl::robin_map<IR::LocationDescriptor, PatchInformation>::const_iterator iter = patch_information.find(target_desc);
    if (iter == patch_information.end()) {
        return;
    }
    const PatchInformation& patch_info = iter->second;
    const auto patch_site = [&](CodePtr site) {
        const tsl::robin_map<CodePtr, LinkSite>::const_iterator link = link_sites.find(site);
        ASSERT(link != link_sites.end());
        StoreRel32(link->second.rel32, target_code_ptr ? target_code_ptr : link->second.stub);
    };
    for (CodePtr site : patch_info.jg) {
        patch_site(site);
    }
    for (CodePtr site : patch_info.jz) {
        patch_site(site);
    }
    for (CodePtr site : patch_info.jmp) {
        patch_site(site);
    }
    const u64 rcx_value = reinterpret_cast<u64>(target_code_ptr ? target_code_ptr : code.GetReturnFromRunCodeAddress());
    for (CodePtr site : patch_info.mov_rcx) {
        u8* const imm64 = const_cast<u8*>(reinterpret_cast<const u8*>(site)) + LinkPadding(site, 2, 8) + 2;
        std::atomic_ref<u64>{*reinterpret_cast<u64*>(imm64)}.store(rcx_value, std::memory_order_release);
    }
}

void A64EmitX64::Unpatch(const IR::LocationDescriptor& location) {
    EmitX64::Unpatch(location);
    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        // Other cores may be reading their tables: only the location is cleared, with one
        // aligned store. The old code stays valid until the whole cache is cleared.
        code.DisableWriting();
        for (FastDispatchTable* table : fast_dispatch_tables) {
            FastDispatchEntry& entry = (*fast_dispatch_table_lookup)(location.Value(), table->data());
            std::atomic_ref<u64>{entry.location_descriptor}.store(0xFFFF'FFFF'FFFF'FFFFull, std::memory_order_relaxed);
        }
        code.EnableWriting();
    }
}

}  // namespace Dynarmic::Backend::X64
