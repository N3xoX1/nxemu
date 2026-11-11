/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/x64/a64_emit_x64.h"

#include <algorithm>
#include <atomic>

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
        : EmitX64(code), conf(conf), jit_interface{jit_interface} {
    GenMemory128Accessors();
    GenFastmemFallbacks();
    GenTerminalHandlers();
    // A quarter of the cache for the cold code of the blocks (fallbacks and link stubs)
    code.ReserveFarCode(conf.code_cache_size / 4);
    code.PreludeComplete();
    ClearFastDispatchTable();

    exception_handler.SetFastmemCallback([this](u64 rip_) {
        return FastmemCallback(rip_);
    });
}

A64EmitX64::~A64EmitX64() = default;

namespace {
bool IsPlainWrite(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::A64WriteMemory8:
    case IR::Opcode::A64WriteMemory16:
    case IR::Opcode::A64WriteMemory32:
    case IR::Opcode::A64WriteMemory64:
    case IR::Opcode::A64WriteMemory128:
        return true;
    default:
        return false;
    }
}

// Ops after which the block may stop before its end: a write before them must be marked
// before them
bool MayLeaveBlock(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::A64CallSupervisor:
    case IR::Opcode::A64SideExit:
    case IR::Opcode::A64ExceptionRaised:
    case IR::Opcode::A64DataCacheOperationRaised:
    case IR::Opcode::A64InstructionCacheOperationRaised:
    case IR::Opcode::A64DataSynchronizationBarrier:
    case IR::Opcode::A64DataMemoryBarrier:
    case IR::Opcode::A64InstructionSynchronizationBarrier:
    case IR::Opcode::A64ClearExclusive:
    case IR::Opcode::A64ExclusiveReadMemory8:
    case IR::Opcode::A64ExclusiveReadMemory16:
    case IR::Opcode::A64ExclusiveReadMemory32:
    case IR::Opcode::A64ExclusiveReadMemory64:
    case IR::Opcode::A64ExclusiveReadMemory128:
    case IR::Opcode::A64ExclusiveWriteMemory8:
    case IR::Opcode::A64ExclusiveWriteMemory16:
    case IR::Opcode::A64ExclusiveWriteMemory32:
    case IR::Opcode::A64ExclusiveWriteMemory64:
    case IR::Opcode::A64ExclusiveWriteMemory128:
        return true;
    default:
        return false;
    }
}

// Splits an address into an IR base value and a constant offset
std::pair<const IR::Inst*, s64> SplitAddress(const IR::Value& address) {
    if (address.IsImmediate()) {
        return {nullptr, 0};
    }
    const IR::Inst* inst = address.GetInstRecursive();
    const auto op = inst->GetOpcode();
    if ((op == IR::Opcode::Add64 || op == IR::Opcode::Sub64) && !inst->GetArg(0).IsImmediate() && inst->GetArg(1).IsImmediate()
        && inst->GetArg(2).IsImmediate() && !inst->GetArg(2).GetU1()) {
        const s64 imm = static_cast<s64>(inst->GetArg(1).GetU64());
        return {inst->GetArg(0).GetInstRecursive(), op == IR::Opcode::Add64 ? imm : -imm};
    }
    return {inst, 0};
}
}  // namespace

// Fastmem writes off the same base value with offsets less than a page apart touch at
// most two pages. In a group of three or more, only the last write marks the write map,
// for the lowest and highest address of the group. The group ends before anything that may
// leave the block, so no write is left unmarked.
void A64EmitX64::PlanWriteTracking(A64EmitContext& ctx) const {
    if (!conf.write_track_map || !conf.fastmem_pointer || conf.check_halt_on_memory_access) {
        return;
    }
    std::vector<std::pair<IR::Inst*, s64>> group;
    const IR::Inst* base = nullptr;
    s64 lowest = 0, highest = 0;
    const auto close = [&] {
        if (group.size() >= 3) {
            const s64 last = group.back().second;
            for (size_t i = 0; i + 1 < group.size(); ++i) {
                ctx.write_track_skip.insert(group[i].first);
            }
            ctx.write_track_group.emplace(group.back().first, std::make_pair(static_cast<s32>(lowest - last), static_cast<s32>(highest - last)));
        }
        group.clear();
        base = nullptr;
    };
    for (auto& inst : ctx.block) {
        const auto op = inst.GetOpcode();
        if (IsPlainWrite(op)) {
            const auto [write_base, offset] = SplitAddress(inst.GetArg(1));
            if (write_base == nullptr || !ShouldFastmem(ctx, &inst)) {
                close();
                continue;
            }
            if (write_base != base || std::max(highest, offset) - std::min(lowest, offset) >= 4096) {
                close();
                base = write_base;
                lowest = highest = offset;
            }
            lowest = std::min(lowest, offset);
            highest = std::max(highest, offset);
            group.emplace_back(&inst, offset);
        } else if (MayLeaveBlock(op)) {
            close();
        }
    }
    close();
}

A64EmitX64::BlockDescriptor A64EmitX64::Emit(IR::Block& block) {
    if (conf.very_verbose_debugging_output) {
        std::puts(IR::DumpBlock(block).c_str());
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
        // Callee-saved registers first: they survive host calls. RAX, RCX and RDX last: many
        // emitters ask for them by name.
        std::stable_partition(gprs.begin(), gprs.end(), [](HostLoc loc) {
            return std::find(ABI_ALL_CALLEE_SAVE.begin(), ABI_ALL_CALLEE_SAVE.end(), loc) != ABI_ALL_CALLEE_SAVE.end();
        });
        std::stable_partition(gprs.begin(), gprs.end(), [](HostLoc loc) {
            return loc != HostLoc::RAX && loc != HostLoc::RCX && loc != HostLoc::RDX;
        });
        return gprs;
    }();

    static const std::vector<HostLoc> xmm_order = [] {
        std::vector<HostLoc> xmms{any_xmm};
        std::stable_partition(xmms.begin(), xmms.end(), [](HostLoc loc) {
            return std::find(ABI_ALL_CALLEE_SAVE.begin(), ABI_ALL_CALLEE_SAVE.end(), loc) != ABI_ALL_CALLEE_SAVE.end();
        });
        return xmms;
    }();
    RegAlloc reg_alloc{code, gpr_order, xmm_order};
    A64EmitContext ctx{conf, reg_alloc, block};
    PlanWriteTracking(ctx);

    // Start emitting.
    code.align();
    const u8* const entrypoint = code.getCurr();

    ASSERT(block.GetCondition() == IR::Cond::AL);

    for (auto iter = block.begin(); iter != block.end(); ++iter) {
        IR::Inst* inst = &*iter;
        const IR::Opcode op = inst->GetOpcode();
        const bool flag_producer = op == IR::Opcode::Add32 || op == IR::Opcode::Add64 || op == IR::Opcode::Sub32 || op == IR::Opcode::Sub64;
        if (flag_producer) {
            ctx.host_flags_nzcv = nullptr;  // set again by the emitter when it makes NZCV
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
            break;
        case IR::Opcode::A64SetNZCV:
            ctx.host_flags_are_cpsr = ctx.host_flags_nzcv && !inst->GetArg(0).IsImmediate() && inst->GetArg(0).GetInst() == ctx.host_flags_nzcv;
            break;
        case IR::Opcode::A64SideExit:
        case IR::Opcode::ConditionalSelect32:
        case IR::Opcode::ConditionalSelect64:
        case IR::Opcode::ConditionalSelectNZCV:
            if (!ctx.host_flags_are_cpsr) {
                ctx.host_flags_nzcv = nullptr;
            }
            break;
        case IR::Opcode::A64GetW:
        case IR::Opcode::A64GetX:
        case IR::Opcode::A64GetSP:
        case IR::Opcode::A64SetW:
        case IR::Opcode::A64SetX:
        case IR::Opcode::A64SetSP:
        case IR::Opcode::Identity:
            break;
        default:
            ctx.host_flags_nzcv = nullptr;
            ctx.host_flags_are_cpsr = false;
            break;
        }

        if (conf.very_verbose_debugging_output) {
            EmitVerboseDebuggingOutput(reg_alloc);
        }
    }

    reg_alloc.AssertNoMoreUses();

    if (conf.enable_cycle_counting) {
        EmitAddCycles(block.CycleCount());
    }
    EmitX64::EmitTerminal(block.GetTerminal(), ctx.Location().SetSingleStepping(false), ctx.IsSingleStep());
    code.int3();
    const size_t size = static_cast<size_t>(code.getCurr() - entrypoint);

    // The cold code goes to the far area, so the hot code of consecutive blocks stays dense
    code.SwitchToFarCode();
    for (auto& deferred_emit : ctx.deferred_emits) {
        deferred_emit();
    }
    EmitLinkStubs();
    code.int3();
    code.SwitchToNearCode();

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

// PC ends up in rbp, location_descriptor ends up in rbx
void A64EmitX64::EmitInlineLocationDescriptor() {
    // This calculation has to match up with A64::LocationDescriptor::UniqueHash
    // TODO: Optimization is available here based on known state of fpcr.
    code.mov(rbp, qword[r15 + offsetof(A64JitState, pc)]);
    code.mov(rcx, A64::LocationDescriptor::pc_mask);
    code.and_(rcx, rbp);
    code.mov(ebx, dword[r15 + offsetof(A64JitState, fpcr)]);
    code.and_(ebx, A64::LocationDescriptor::fpcr_mask);
    code.shl(rbx, A64::LocationDescriptor::fpcr_shift);
    code.or_(rbx, rcx);
}

void A64EmitX64::GenTerminalHandlers() {
    const auto calculate_location_descriptor = [this] { EmitInlineLocationDescriptor(); };

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
        terminal_handler_rsb_miss = code.getCurr<const void*>();
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
        terminal_handler_fast_dispatch_miss = code.getCurr<const void*>();
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
}

void A64EmitX64::EmitA64SetNZCV(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const Xbyak::Reg32 to_store = ctx.reg_alloc.UseScratchGpr(args[0]).cvt32();
    code.mov(dword[r15 + offsetof(A64JitState, cpsr_nzcv)], to_store);
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

// Ops whose emitters always write a 32-bit GPR (or zero-extend their fallback result), so the
// upper half of the host register is clear wherever the value is moved afterwards.
void A64EmitX64::EmitA64SetW(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Reg reg = inst->GetArg(0).GetA64RegRef();
    const auto addr = qword[r15 + offsetof(A64JitState, reg) + sizeof(u64) * static_cast<size_t>(reg)];
    if (args[1].FitsInImmediateS32()) {
        code.mov(addr, args[1].GetImmediateS32());
    } else if (args[1].IsImmediate()) {
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

void A64EmitX64::EmitA64SetS(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const A64::Vec vec = inst->GetArg(0).GetA64VecRef();
    const auto addr = xword[r15 + offsetof(A64JitState, vec) + sizeof(u64) * 2 * static_cast<size_t>(vec)];

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

void A64EmitX64::EmitA64SideExit(A64EmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const IR::Cond cond = args[0].GetImmediateCond();
    const IR::LocationDescriptor next = ctx.Location().SetPC(args[2].GetImmediateU64());
    const size_t cycles = static_cast<size_t>(args[3].GetImmediateU64());
    const IR::LocationDescriptor location = ctx.Location().SetSingleStepping(false);
    const bool is_single_step = ctx.IsSingleStep();

    SharedLabel exit = GenSharedLabel();
    if (cond == IR::Cond::AL || cond == IR::Cond::NV) {
        const bool exit_when_set = cond == IR::Cond::AL;
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
    } else {
        if (ctx.host_flags_are_cpsr) {
            // The host flags already hold the guest NZCV. Only HI and LS need CF inverted.
            if (cond == IR::Cond::HI || cond == IR::Cond::LS) {
                code.cmc();
                ctx.host_flags_are_cpsr = false;
            }
        } else {
            ctx.reg_alloc.ScratchGpr(HostLoc::RAX);
            code.mov(eax, dword[r15 + offsetof(A64JitState, cpsr_nzcv)]);
            code.LoadRequiredFlagsForCondFromRax(cond);
        }
        switch (cond) {
        case IR::Cond::EQ: code.jz(*exit, code.T_NEAR); break;
        case IR::Cond::NE: code.jnz(*exit, code.T_NEAR); break;
        case IR::Cond::CS: code.jc(*exit, code.T_NEAR); break;
        case IR::Cond::CC: code.jnc(*exit, code.T_NEAR); break;
        case IR::Cond::MI: code.js(*exit, code.T_NEAR); break;
        case IR::Cond::PL: code.jns(*exit, code.T_NEAR); break;
        case IR::Cond::VS: code.jo(*exit, code.T_NEAR); break;
        case IR::Cond::VC: code.jno(*exit, code.T_NEAR); break;
        case IR::Cond::HI: code.ja(*exit, code.T_NEAR); break;
        case IR::Cond::LS: code.jna(*exit, code.T_NEAR); break;
        case IR::Cond::GE: code.jge(*exit, code.T_NEAR); break;
        case IR::Cond::LT: code.jl(*exit, code.T_NEAR); break;
        case IR::Cond::GT: code.jg(*exit, code.T_NEAR); break;
        case IR::Cond::LE: code.jle(*exit, code.T_NEAR); break;
        default: ASSERT_MSG(false, "Unknown cond {}", static_cast<size_t>(cond)); break;
        }
    }

    // The guest registers are already stored, so the exit only has to count its cycles
    // and link to the target block.
    ctx.deferred_emits.emplace_back([=, this] {
        code.L(*exit);
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

    // Each block gets its own copy of the lookup and its own indirect jump, which the host
    // predicts per site. A miss takes the shared path with the same registers.
    if (!terminal_handler_rsb_miss) {
        code.jmp(terminal_handler_pop_rsb_hint);
        return;
    }
    EmitInlineLocationDescriptor();
    code.mov(eax, dword[r15 + offsetof(A64JitState, rsb_ptr)]);
    code.sub(eax, 1);
    code.and_(eax, u32(A64JitState::RSBPtrMask));
    code.mov(dword[r15 + offsetof(A64JitState, rsb_ptr)], eax);
    code.cmp(rbx, qword[r15 + offsetof(A64JitState, rsb_location_descriptors) + rax * sizeof(u64)]);
    code.jne(terminal_handler_rsb_miss);
    code.jmp(qword[r15 + offsetof(A64JitState, rsb_codeptrs) + rax * sizeof(u64)]);
}

void A64EmitX64::EmitTerminalImpl(IR::Term::FastDispatchHint, IR::LocationDescriptor, bool is_single_step) {
    if (!conf.HasOptimization(OptimizationFlag::FastDispatch) || is_single_step) {
        code.ReturnFromRunCode();
        return;
    }

    if (!terminal_handler_fast_dispatch_miss) {
        code.jmp(terminal_handler_fast_dispatch_hint);
        return;
    }
    EmitInlineLocationDescriptor();
    code.mov(r12, qword[r15 + offsetof(A64JitState, core_fast_dispatch_table)]);
    code.mov(rbp, rbx);
    if (code.HasHostFeature(HostFeature::SSE42)) {
        code.crc32(rbp, r12);
    }
    code.and_(ebp, fast_dispatch_table_mask);
    code.lea(rbp, ptr[r12 + rbp]);
    code.cmp(rbx, qword[rbp + offsetof(FastDispatchEntry, location_descriptor)]);
    code.jne(terminal_handler_fast_dispatch_miss);
    code.jmp(ptr[rbp + offsetof(FastDispatchEntry, code_ptr)]);
}

void A64EmitX64::EmitTerminalImpl(IR::Term::If terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    switch (terminal.if_) {
    case IR::Cond::AL:
    case IR::Cond::NV:
        EmitTerminal(terminal.then_, initial_location, is_single_step);
        break;
    default:
        Xbyak::Label pass = EmitCond(terminal.if_);
        EmitTerminal(terminal.else_, initial_location, is_single_step);
        code.L(pass);
        EmitTerminal(terminal.then_, initial_location, is_single_step);
        break;
    }
}

void A64EmitX64::EmitTerminalImpl(IR::Term::CheckBit terminal, IR::LocationDescriptor initial_location, bool is_single_step) {
    Xbyak::Label fail;
    code.cmp(code.byte[rsp + ABI_SHADOW_SPACE + offsetof(StackLayout, check_bit)], u8(0));
    code.jz(fail);
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
    const auto field = reinterpret_cast<uintptr_t>(site) + opcode_size;
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
    EmitLinkSite(0x8F, target_desc, target_code_ptr);
}

void A64EmitX64::EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    EmitLinkSite(0x84, target_desc, target_code_ptr);
}

void A64EmitX64::EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    EmitLinkSite(0, target_desc, target_code_ptr);
}

void A64EmitX64::EmitPatchMovRcx(CodePtr target_code_ptr) {
    if (!target_code_ptr) {
        target_code_ptr = code.GetReturnFromRunCodeAddress();
    }
    // mov rcx, imm64 in its 10-byte form, imm64 aligned on 8
    code.nop(LinkPadding(code.getCurr(), 2, 8));
    code.db(0x48);
    code.db(0xB9);
    code.dq(reinterpret_cast<u64>(target_code_ptr));
}

void A64EmitX64::Patch(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) {
    const auto iter = patch_information.find(target_desc);
    if (iter == patch_information.end()) {
        return;
    }
    const PatchInformation& patch_info = iter->second;
    const auto patch_site = [&](CodePtr site) {
        const auto link = link_sites.find(site);
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
        // Other cores may be running and reading their own table: only the location is
        // cleared (one aligned store), so a core that already matched still jumps to the
        // old code, which stays valid until the whole cache is cleared.
        for (FastDispatchTable* table : fast_dispatch_tables) {
            FastDispatchEntry& entry = (*fast_dispatch_table_lookup)(location.Value(), table->data());
            std::atomic_ref<u64>{entry.location_descriptor}.store(0xFFFF'FFFF'FFFF'FFFFull, std::memory_order_relaxed);
        }
    }
}

}  // namespace Dynarmic::Backend::X64
