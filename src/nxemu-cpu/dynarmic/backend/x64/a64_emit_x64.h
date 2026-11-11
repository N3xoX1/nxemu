/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <map>
#include <optional>
#include <tuple>

#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/emit_x64.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::X64 {

class RegAlloc;

struct A64EmitContext final : public EmitContext {
    A64EmitContext(const A64::UserConfig& conf, RegAlloc& reg_alloc, IR::Block& block);

    A64::LocationDescriptor Location() const;
    bool IsSingleStep() const;
    FP::FPCR FPCR(bool fpcr_controlled = true) const override;

    bool HasOptimization(OptimizationFlag flag) const override {
        return conf.HasOptimization(flag);
    }

    const A64::UserConfig& conf;
};

class A64EmitX64 final : public EmitX64 {
public:
    A64EmitX64(BlockOfCode& code, A64::UserConfig conf, A64::Jit* jit_interface);
    ~A64EmitX64() override;

    /**
     * Emit host machine code for a basic block with intermediate representation `block`.
     * @note block is modified.
     */
    BlockDescriptor Emit(IR::Block& block);

    void ClearCache() override;

    void InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges);

    struct FastDispatchEntry {
        u64 location_descriptor = 0xFFFF'FFFF'FFFF'FFFFull;
        const void* code_ptr = nullptr;
    };
    static_assert(sizeof(FastDispatchEntry) == 0x10);
    static constexpr u64 fast_dispatch_table_mask = 0xFFFFF0;
    static constexpr size_t fast_dispatch_table_size = 0x100000;
    using FastDispatchTable = std::array<FastDispatchEntry, fast_dispatch_table_size>;

    /// Each core that runs this code has its own fast dispatch table (its address is in the
    /// core's JIT state). Invalidation clears the entries of every registered table.
    void AddFastDispatchTable(FastDispatchTable* table);
    void RemoveFastDispatchTable(FastDispatchTable* table);

    /// Code of `location` in a core's table, or null. Called by that core only.
    const void* LookupFastDispatch(FastDispatchTable* table, u64 location) const {
        if (!fast_dispatch_table_lookup) {
            return nullptr;
        }
        const FastDispatchEntry& entry = fast_dispatch_table_lookup(location, table->data());
        return entry.location_descriptor == location ? entry.code_ptr : nullptr;
    }
    /// Called by the table's core while it holds cache_mutex, so no invalidation runs meanwhile.
    void InsertFastDispatch(FastDispatchTable* table, u64 location, const void* code_ptr) const {
        if (!fast_dispatch_table_lookup) {
            return;
        }
        FastDispatchEntry& entry = fast_dispatch_table_lookup(location, table->data());
        entry.code_ptr = code_ptr;
        entry.location_descriptor = location;
    }

protected:
    /// A user callback whose object comes from the running core's JIT state.
    template<auto mfp>
    ArgCallback CoreCallback() const {
        return DevirtualizeFromState<mfp>(conf.callbacks, offsetof(A64JitState, core_callbacks));
    }

    const A64::UserConfig conf;
    A64::Jit* jit_interface;
    BlockRangeInformation<u64> block_ranges;

    std::vector<FastDispatchTable*> fast_dispatch_tables;
    void ClearFastDispatchTable();

    void (*memory_read_128)();
    void (*memory_write_128)();
    void (*memory_exclusive_write_128)();
    void GenMemory128Accessors();

    std::map<std::tuple<bool, size_t, int, int>, void (*)()> read_fallbacks;
    std::map<std::tuple<bool, size_t, int, int>, void (*)()> write_fallbacks;
    std::map<std::tuple<bool, size_t, int, int>, void (*)()> exclusive_write_fallbacks;
    void GenFastmemFallbacks();

    const void* terminal_handler_pop_rsb_hint;
    const void* terminal_handler_fast_dispatch_hint = nullptr;
    const void* terminal_handler_rsb_miss = nullptr;            // expects the location descriptor in rbx
    const void* terminal_handler_fast_dispatch_miss = nullptr;  // expects rbx and the table entry in rbp
    void EmitInlineLocationDescriptor();
    FastDispatchEntry& (*fast_dispatch_table_lookup)(u64, FastDispatchEntry*) = nullptr;
    void GenTerminalHandlers();

    // Microinstruction emitters
    void EmitPushRSB(EmitContext& ctx, IR::Inst* inst);
#define OPCODE(...)
#define A32OPC(...)
#define A64OPC(name, type, ...) void EmitA64##name(A64EmitContext& ctx, IR::Inst* inst);
#include "dynarmic/ir/opcodes.inc"
#undef OPCODE
#undef A32OPC
#undef A64OPC

    // Helpers
    std::string LocationDescriptorToFriendlyName(const IR::LocationDescriptor&) const override;

    // Fastmem information
    using DoNotFastmemMarker = std::tuple<IR::LocationDescriptor, unsigned>;
    struct FastmemPatchInfo {
        u64 resume_rip;
        u64 callback;
        DoNotFastmemMarker marker;
        bool recompile;
    };
    tsl::robin_map<u64, FastmemPatchInfo> fastmem_patch_info;
    std::set<DoNotFastmemMarker> do_not_fastmem;
    std::optional<DoNotFastmemMarker> ShouldFastmem(A64EmitContext& ctx, IR::Inst* inst) const;
    void PlanWriteTracking(A64EmitContext& ctx) const;
    FakeCall FastmemCallback(u64 rip);

    // Memory access helpers
    void EmitCheckMemoryAbort(A64EmitContext& ctx, IR::Inst* inst, Xbyak::Label* end = nullptr);
    template<std::size_t bitsize, auto callback>
    void EmitMemoryRead(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitMemoryWrite(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveReadMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveWriteMemory(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveReadMemoryInline(A64EmitContext& ctx, IR::Inst* inst);
    template<std::size_t bitsize, auto callback>
    void EmitExclusiveWriteMemoryInline(A64EmitContext& ctx, IR::Inst* inst);

    // Terminal instruction emitters
    void EmitTerminalImpl(IR::Term::Interpret terminal, IR::LocationDescriptor initial_location, bool is_single_step) override;
    void EmitTerminalImpl(IR::Term::ReturnToDispatch terminal, IR::LocationDescriptor initial_location, bool is_single_step) override;
    void EmitTerminalImpl(IR::Term::LinkBlock terminal, IR::LocationDescriptor initial_location, bool is_single_step) override;
    void EmitTerminalImpl(IR::Term::LinkBlockFast terminal, IR::LocationDescriptor initial_location, bool is_single_step) override;
    void EmitTerminalImpl(IR::Term::PopRSBHint terminal, IR::LocationDescriptor initial_location, bool is_single_step) override;
    void EmitTerminalImpl(IR::Term::FastDispatchHint terminal, IR::LocationDescriptor initial_location, bool is_single_step) override;
    void EmitTerminalImpl(IR::Term::If terminal, IR::LocationDescriptor initial_location, bool is_single_step) override;
    void EmitTerminalImpl(IR::Term::CheckBit terminal, IR::LocationDescriptor initial_location, bool is_single_step) override;
    void EmitTerminalImpl(IR::Term::CheckHalt terminal, IR::LocationDescriptor initial_location, bool is_single_step) override;

    // Patching
    // Block links are changed while the code may run (on another core once the cache is
    // shared). A link site is one jg/jz/jmp rel32 whose rel32 is 4-byte aligned, so linking and
    // unlinking is one aligned store. Unlinked, it goes to a stub after the block that stores the
    // target PC and returns to the dispatcher. The RSB `mov rcx, imm64` has its imm64 8-byte aligned.
    struct PendingLinkStub {
        CodePtr site;
        u8* rel32;
        u64 target_pc;
        bool unlinked;
    };
    struct LinkSite {
        u8* rel32;
        CodePtr stub;
    };
    std::vector<PendingLinkStub> pending_link_stubs;
    tsl::robin_map<CodePtr, LinkSite> link_sites;
    void EmitLinkSite(u8 condition_opcode, const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr);
    void EmitLinkStubs();

    void Patch(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr) override;
    void Unpatch(const IR::LocationDescriptor& target_desc) override;
    void EmitPatchJg(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchJz(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchJmp(const IR::LocationDescriptor& target_desc, CodePtr target_code_ptr = nullptr) override;
    void EmitPatchMovRcx(CodePtr target_code_ptr = nullptr) override;
};

}  // namespace Dynarmic::Backend::X64
