/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include <boost/icl/interval_set.hpp>
#include <mcl/assert.hpp>
#include <mcl/bit_cast.hpp>
#include <mcl/scope_exit.hpp>

#include "dynarmic/backend/x64/a64_emit_x64.h"
#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/exclusive_monitor_friend.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/common/x64_disassemble.h"
#include "dynarmic/frontend/A64/translate/a64_translate.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt/passes.h"

namespace Dynarmic::A64 {

using namespace Backend::X64;

// The dispatcher reads the core's Jit::Impl and callbacks from the JIT state, so the run code
// does not depend on the core.
static RunCodeCallbacks GenRunCodeCallbacks(A64::UserCallbacks* cb, CodePtr (*LookupBlock)(void* lookup_block_arg), void* arg, const A64::UserConfig& conf) {
    return RunCodeCallbacks{
        std::make_unique<ArgCallback>(ArgCallback{LookupBlock, reinterpret_cast<u64>(arg)}.WithArgFromState(offsetof(A64JitState, core_lookup_arg))),
        std::make_unique<ArgCallback>(DevirtualizeFromState<&A64::UserCallbacks::AddTicks>(cb, offsetof(A64JitState, core_callbacks))),
        std::make_unique<ArgCallback>(DevirtualizeFromState<&A64::UserCallbacks::GetTicksRemaining>(cb, offsetof(A64JitState, core_callbacks))),
        conf.enable_cycle_counting,
    };
}

static std::function<void(BlockOfCode&)> GenRCP(const A64::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, mcl::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.mov(code.r13, mcl::bit_cast<u64>(conf.fastmem_pointer));
        }
    };
}

static Optimization::PolyfillOptions GenPolyfillOptions(const BlockOfCode& code) {
    return Optimization::PolyfillOptions{
        .sha256 = !code.HasHostFeature(HostFeature::SHA),
        .vector_multiply_widen = true,
    };
}

/// True when two cores can run the same generated code: everything but the per-core values
/// (callbacks, processor id, TPIDR pointers), which the code reads from the JIT state.
static bool SameGeneratedCode(const UserConfig& a, const UserConfig& b) {
    return a.global_monitor == b.global_monitor && a.optimizations == b.optimizations
        && a.unsafe_optimizations == b.unsafe_optimizations && a.hook_data_cache_operations == b.hook_data_cache_operations
        && a.hook_isb == b.hook_isb && a.hook_hint_instructions == b.hook_hint_instructions
        && a.cntfrq_el0 == b.cntfrq_el0 && a.ctr_el0 == b.ctr_el0 && a.dczid_el0 == b.dczid_el0
        && (a.tpidr_el0 == nullptr) == (b.tpidr_el0 == nullptr) && (a.tpidrro_el0 == nullptr) == (b.tpidrro_el0 == nullptr)
        && a.page_table == b.page_table && a.page_table_address_space_bits == b.page_table_address_space_bits
        && a.page_table_pointer_mask_bits == b.page_table_pointer_mask_bits && a.silently_mirror_page_table == b.silently_mirror_page_table
        && a.absolute_offset_page_table == b.absolute_offset_page_table
        && a.detect_misaligned_access_via_page_table == b.detect_misaligned_access_via_page_table
        && a.only_detect_misalignment_via_page_table_on_page_boundary == b.only_detect_misalignment_via_page_table_on_page_boundary
        && a.fastmem_pointer == b.fastmem_pointer && a.recompile_on_fastmem_failure == b.recompile_on_fastmem_failure
        && a.fastmem_address_space_bits == b.fastmem_address_space_bits && a.silently_mirror_fastmem == b.silently_mirror_fastmem
        && a.fastmem_exclusive_access == b.fastmem_exclusive_access
        && a.recompile_on_exclusive_fastmem_failure == b.recompile_on_exclusive_fastmem_failure
        && a.write_track_map == b.write_track_map && a.write_track_l2_offset == b.write_track_l2_offset
        && a.write_track_l1_offset == b.write_track_l1_offset && a.define_unpredictable_behaviour == b.define_unpredictable_behaviour
        && a.wall_clock_cntpct == b.wall_clock_cntpct && a.check_halt_on_memory_access == b.check_halt_on_memory_access
        && a.enable_cycle_counting == b.enable_cycle_counting && a.code_cache_size == b.code_cache_size
        && a.very_verbose_debugging_output == b.very_verbose_debugging_output;
}

/// Generated code shared by the cores of one process: one code buffer and one set of block
/// maps, so a block is compiled once and the code footprint is not duplicated per core. Each
/// core keeps its own JIT state, RSB and fast dispatch table.
///
/// Locks, in nesting order: apply_mutex, emitter.cache_mutex, cores_mutex, invalidation_mutex.
/// - emitter.cache_mutex: shared for lookups, exclusive to compile, link or invalidate.
/// - Range invalidation does not stop the other cores: the old code is never freed, links are
///   unlinked with one aligned store, and every core is halted so it drops its RSB once the
///   ranges are applied (apply_mutex makes it wait for an application in progress).
/// - A full clear reuses the code memory, so it waits until no core runs JIT code.
struct SharedCodeCache {
    /// What the cache needs from each attached core.
    struct Core {
        alignas(64) std::atomic<bool> in_jit{false};  // the core may run generated code
        A64JitState* jit_state = nullptr;
    };

    SharedCodeCache(Jit* jit, const UserConfig& conf, CodePtr (*lookup_block)(void*))
            : conf(conf)
            , block_of_code(GenRunCodeCallbacks(conf.callbacks, lookup_block, nullptr, conf), JitStateInfo{A64JitState{}}, conf.code_cache_size, GenRCP(conf))
            , emitter(block_of_code, conf, jit)
            , polyfill_options(GenPolyfillOptions(block_of_code)) {}

    void Attach(Core* core, A64EmitX64::FastDispatchTable* table);
    void Detach(Core* core, A64EmitX64::FastDispatchTable* table);
    void RequestRanges(u64 start_address, size_t length);
    void RequestClear();
    void ApplyRanges();
    void ApplyClear();
    void HaltAll();

    const UserConfig conf;
    BlockOfCode block_of_code;
    A64EmitX64 emitter;
    Optimization::PolyfillOptions polyfill_options;
    IR::Block ir_block{IR::LocationDescriptor{0}};  // guarded by emitter.cache_mutex

    std::mutex apply_mutex;
    std::mutex cores_mutex;
    std::vector<Core*> cores;
    std::mutex invalidation_mutex;
    boost::icl::interval_set<u64> invalid_cache_ranges;
    std::atomic<bool> clear_pending{false};
};

namespace {
std::mutex shared_caches_mutex;
std::vector<std::weak_ptr<SharedCodeCache>> shared_caches;

std::shared_ptr<SharedCodeCache> AcquireSharedCodeCache(Jit* jit, const UserConfig& conf, CodePtr (*lookup_block)(void*)) {
    std::scoped_lock lock{shared_caches_mutex};
    std::erase_if(shared_caches, [](const auto& weak) { return weak.expired(); });
    for (const auto& weak : shared_caches) {
        if (auto cache = weak.lock(); cache && SameGeneratedCode(cache->conf, conf)) {
            return cache;
        }
    }
    auto cache = std::make_shared<SharedCodeCache>(jit, conf, lookup_block);
    shared_caches.push_back(cache);
    return cache;
}
}  // namespace

struct Jit::Impl final {
public:
    Impl(Jit* jit, UserConfig conf)
            : conf(conf)
            , shared(AcquireSharedCodeCache(jit, conf, &LookupBlockThunk))
            , block_of_code(shared->block_of_code)
            , emitter(shared->emitter)
            , fast_dispatch_table(std::make_unique<A64EmitX64::FastDispatchTable>()) {
        ASSERT(conf.page_table_address_space_bits >= 12 && conf.page_table_address_space_bits <= 64);
        core_link.jit_state = &jit_state;
        shared->Attach(&core_link, fast_dispatch_table.get());
        SetCoreContext();
    }

    ~Impl() {
        shared->Detach(&core_link, fast_dispatch_table.get());
    }

    HaltReason Run() {
        ASSERT(!is_executing);
        HandleHalt(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));

        is_executing = true;
        SCOPE_EXIT {
            this->is_executing = false;
        };

        HaltReason hr;
        {
            EnterJit();
            SCOPE_EXIT {
                LeaveJit();
            };

            const CodePtr current_code_ptr = [this] {
                // RSB optimization
                const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & A64JitState::RSBPtrMask;
                if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                    jit_state.rsb_ptr = new_rsb_ptr;
                    return reinterpret_cast<CodePtr>(jit_state.rsb_codeptrs[new_rsb_ptr]);
                }

                return GetCurrentBlock();
            }();

            hr = block_of_code.RunCode(&jit_state, current_code_ptr);
        }

        HandleHalt(hr);

        return hr;
    }

    HaltReason Step() {
        ASSERT(!is_executing);
        HandleHalt(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));

        is_executing = true;
        SCOPE_EXIT {
            this->is_executing = false;
        };

        HaltReason hr;
        {
            EnterJit();
            SCOPE_EXIT {
                LeaveJit();
            };
            hr = block_of_code.StepCode(&jit_state, GetCurrentSingleStep());
        }

        HandleHalt(hr);

        return hr;
    }

    void ClearCache() {
        shared->RequestClear();
    }

    void InvalidateCacheRange(u64 start_address, size_t length) {
        shared->RequestRanges(start_address, length);
    }

    void Reset() {
        ASSERT(!is_executing);
        jit_state = {};
        SetCoreContext();
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }


    u64 GetSP() const {
        return jit_state.sp;
    }

    void SetSP(u64 value) {
        jit_state.sp = value;
    }

    u64 GetPC() const {
        return jit_state.pc;
    }

    void SetPC(u64 value) {
        jit_state.pc = value;
    }

    u64 GetRegister(size_t index) const {
        if (index == 31)
            return GetSP();
        return jit_state.reg.at(index);
    }

    void SetRegister(size_t index, u64 value) {
        if (index == 31)
            return SetSP(value);
        jit_state.reg.at(index) = value;
    }

    std::array<u64, 31> GetRegisters() const {
        return jit_state.reg;
    }

    void SetRegisters(const std::array<u64, 31>& value) {
        jit_state.reg = value;
    }

    Vector GetVector(size_t index) const {
        return {jit_state.vec.at(index * 2), jit_state.vec.at(index * 2 + 1)};
    }

    void SetVector(size_t index, Vector value) {
        jit_state.vec.at(index * 2) = value[0];
        jit_state.vec.at(index * 2 + 1) = value[1];
    }

    uint64_t * GetVectorData()
    {
        return jit_state.vec.data();
    }

    std::array<Vector, 32> GetVectors() const
    {
        std::array<Vector, 32> ret;
        static_assert(sizeof(ret) == sizeof(jit_state.vec));
        std::memcpy(ret.data(), jit_state.vec.data(), sizeof(jit_state.vec));
        return ret;
    }

    void SetVectors(const std::array<Vector, 32>& value) {
        static_assert(sizeof(value) == sizeof(jit_state.vec));
        std::memcpy(jit_state.vec.data(), value.data(), sizeof(jit_state.vec));
    }

    u32 GetFpcr() const {
        return jit_state.GetFpcr();
    }

    void SetFpcr(u32 value) {
        jit_state.SetFpcr(value);
    }

    u32 GetFpsr() const {
        return jit_state.GetFpsr();
    }

    void SetFpsr(u32 value) {
        jit_state.SetFpsr(value);
    }

    u32 GetPstate() const {
        return jit_state.GetPstate();
    }

    void SetPstate(u32 value) {
        jit_state.SetPstate(value);
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    bool IsExecuting() const {
        return is_executing;
    }

    void DumpDisassembly() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        Common::DumpDisassembledX64(block_of_code.GetCodeBegin(), size);
    }

    std::vector<std::string> Disassemble() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        return Common::DisassembleX64(block_of_code.GetCodeBegin(), size);
    }



private:
    /// Called by the dispatcher after a fast dispatch miss. That path has already written the
    /// location in the table entry, so the table is not probed here.
    static CodePtr LookupBlockThunk(void* impl) {
        Jit::Impl* this_ = static_cast<Jit::Impl*>(impl);
        return this_->GetBlock(this_->GetCurrentLocation(), false);
    }

    void SetCoreContext() {
        jit_state.core_callbacks = reinterpret_cast<u64>(conf.callbacks);
        jit_state.core_lookup_arg = reinterpret_cast<u64>(this);
        jit_state.core_monitor = reinterpret_cast<u64>(conf.global_monitor);
        if (conf.global_monitor) {
            jit_state.core_excl_addr_ptr = reinterpret_cast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id));
            jit_state.core_excl_value_ptr = reinterpret_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id));
        }
        jit_state.core_tpidr_el0_ptr = reinterpret_cast<u64>(conf.tpidr_el0);
        jit_state.core_tpidrro_el0_ptr = reinterpret_cast<u64>(conf.tpidrro_el0);
        jit_state.core_fast_dispatch_table = reinterpret_cast<u64>(fast_dispatch_table->data());
        jit_state.core_processor_id = conf.processor_id;
    }

    /// Marks this core as running generated code. A full clear waits for every core to be out,
    /// so a pending one is done (or waited for) here first.
    void EnterJit() {
        for (;;) {
            core_link.in_jit.store(true, std::memory_order_seq_cst);
            if (!shared->clear_pending.load(std::memory_order_seq_cst)) {
                return;
            }
            core_link.in_jit.store(false, std::memory_order_seq_cst);
            shared->ApplyClear();
            jit_state.ResetRSB();
        }
    }

    void LeaveJit() {
        core_link.in_jit.store(false, std::memory_order_release);
    }

    void HandleHalt(HaltReason hr) {
        if (!Has(hr, HaltReason::CacheInvalidation)) {
            return;
        }
        ClearHalt(HaltReason::CacheInvalidation);
        if (shared->clear_pending.load(std::memory_order_seq_cst)) {
            shared->ApplyClear();
        }
        // Applies the pending ranges, or waits for the core that is applying them, so the RSB
        // is dropped after the old blocks are gone.
        shared->ApplyRanges();
        jit_state.ResetRSB();
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    CodePtr GetCurrentBlock() {
        return GetBlock(GetCurrentLocation(), true);
    }

    CodePtr GetCurrentSingleStep() {
        return GetBlock(A64::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true), true);
    }

    CodePtr GetBlock(IR::LocationDescriptor current_location, bool probe_table) {
        // The core's own fast dispatch table first: no shared cache line is touched.
        if (const void* code_ptr = probe_table ? emitter.LookupFastDispatch(fast_dispatch_table.get(), current_location.Value()) : nullptr) {
            return static_cast<CodePtr>(code_ptr);
        }
        {
            std::shared_lock lock{emitter.cache_mutex};
            if (auto block = emitter.GetBasicBlock(current_location)) {
                emitter.InsertFastDispatch(fast_dispatch_table.get(), current_location.Value(), block->entrypoint);
                return block->entrypoint;
            }
        }

        if (conf.notify_compilation) conf.callbacks->OnCompilation(true);
        SCOPE_EXIT { if (conf.notify_compilation) conf.callbacks->OnCompilation(false); };

        std::unique_lock lock{emitter.cache_mutex};
        if (auto block = emitter.GetBasicBlock(current_location)) {
            // Another core compiled it meanwhile.
            emitter.InsertFastDispatch(fast_dispatch_table.get(), current_location.Value(), block->entrypoint);
            return block->entrypoint;
        }

        // The cache can only be cleared once every core is out of JIT code, so the clear is
        // requested early and the compilation goes on in the space that is left.
        constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;
        if (block_of_code.SpaceRemaining() < shared->conf.code_cache_size / 8 && !shared->clear_pending.load()) {
            shared->RequestClear();
        }
        ASSERT_MSG(block_of_code.SpaceRemaining() >= MINIMUM_REMAINING_CODESIZE, "JIT code cache full");
        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);

        // JIT Compile. Reuse the IR block so cold compilation does not allocate a large
        // instruction arena for every guest block.
        IR::Block& ir_block = shared->ir_block;
        const auto get_code = [this](u64 vaddr) { return conf.callbacks->MemoryReadCode(vaddr); };
        const auto descriptor = A64::LocationDescriptor{current_location};
        ir_block.Reset(descriptor);
        A64::TranslationOptions options{conf.define_unpredictable_behaviour, conf.wall_clock_cntpct};
        options.side_exits = conf.HasOptimization(OptimizationFlag::BlockLinking);
        A64::Translate(ir_block, descriptor, get_code, options);
        Optimization::PolyfillPass(ir_block, shared->polyfill_options);
        Optimization::A64CallbackConfigPass(ir_block, conf);
        Optimization::NamingPass(ir_block);
        if (conf.HasOptimization(OptimizationFlag::GetSetElimination) && !conf.check_halt_on_memory_access) {
            Optimization::A64GetSetElimination(ir_block);
            Optimization::DeadCodeElimination(ir_block);
        }
        if (conf.HasOptimization(OptimizationFlag::ConstProp)) {
            Optimization::ConstantPropagation(ir_block);
            Optimization::DeadCodeElimination(ir_block);
        }
        if (conf.HasOptimization(OptimizationFlag::MiscIROpt)) {
            Optimization::A64MergeInterpretBlocksPass(ir_block, conf.callbacks);
        }
        if (!conf.HasOptimization(OptimizationFlag::DisableVerification)) {
            Optimization::VerificationPass(ir_block);
        }
        const CodePtr entrypoint = emitter.Emit(ir_block).entrypoint;
        emitter.InsertFastDispatch(fast_dispatch_table.get(), current_location.Value(), entrypoint);
        return entrypoint;
    }

    bool is_executing = false;
    SharedCodeCache::Core core_link;

    const UserConfig conf;
    alignas(64) A64JitState jit_state;
    std::shared_ptr<SharedCodeCache> shared;
    BlockOfCode& block_of_code;
    A64EmitX64& emitter;
    std::unique_ptr<A64EmitX64::FastDispatchTable> fast_dispatch_table;
};

void SharedCodeCache::Attach(Core* core, A64EmitX64::FastDispatchTable* table) {
    std::unique_lock cache_lock{emitter.cache_mutex};
    std::scoped_lock cores_lock{cores_mutex};
    emitter.AddFastDispatchTable(table);
    cores.push_back(core);
}

void SharedCodeCache::Detach(Core* core, A64EmitX64::FastDispatchTable* table) {
    std::scoped_lock apply_lock{apply_mutex};
    std::unique_lock cache_lock{emitter.cache_mutex};
    std::scoped_lock cores_lock{cores_mutex};
    emitter.RemoveFastDispatchTable(table);
    std::erase(cores, core);
}

void SharedCodeCache::HaltAll() {
    std::scoped_lock lock{cores_mutex};
    for (Core* core : cores) {
        Atomic::Or(&core->jit_state->halt_reason, static_cast<u32>(HaltReason::CacheInvalidation));
    }
}

void SharedCodeCache::RequestRanges(u64 start_address, size_t length) {
    {
        std::scoped_lock lock{invalidation_mutex};
        const auto end_address = static_cast<u64>(start_address + length - 1);
        invalid_cache_ranges.add(boost::icl::discrete_interval<u64>::closed(start_address, end_address));
    }
    HaltAll();
}

void SharedCodeCache::RequestClear() {
    clear_pending.store(true, std::memory_order_seq_cst);
    HaltAll();
}

void SharedCodeCache::ApplyRanges() {
    std::scoped_lock apply_lock{apply_mutex};
    boost::icl::interval_set<u64> ranges;
    {
        std::scoped_lock lock{invalidation_mutex};
        ranges.swap(invalid_cache_ranges);
    }
    if (ranges.empty()) {
        return;
    }
    std::unique_lock cache_lock{emitter.cache_mutex};
    emitter.InvalidateCacheRanges(ranges);
}

void SharedCodeCache::ApplyClear() {
    std::scoped_lock apply_lock{apply_mutex};
    if (!clear_pending.load(std::memory_order_seq_cst)) {
        return;
    }
    // Every core is halted; wait until none runs JIT code. Detach also takes apply_mutex, so
    // the list stays valid without holding cores_mutex (a running core may need it to halt).
    std::vector<Core*> snapshot;
    {
        std::scoped_lock lock{cores_mutex};
        snapshot = cores;
    }
    for (Core* core : snapshot) {
        while (core->in_jit.load(std::memory_order_seq_cst)) {
            std::this_thread::yield();
        }
    }
    std::unique_lock cache_lock{emitter.cache_mutex};
    {
        std::scoped_lock lock{invalidation_mutex};
        invalid_cache_ranges.clear();
    }
    block_of_code.ClearCache();
    emitter.ClearCache();
    clear_pending.store(false, std::memory_order_seq_cst);
}

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Jit::Impl>(this, conf)) {}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(u64 start_address, size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

u64 Jit::GetSP() const {
    return impl->GetSP();
}

void Jit::SetSP(u64 value) {
    impl->SetSP(value);
}

u64 Jit::GetPC() const {
    return impl->GetPC();
}

void Jit::SetPC(u64 value) {
    impl->SetPC(value);
}

u64 Jit::GetRegister(size_t index) const {
    return impl->GetRegister(index);
}

void Jit::SetRegister(size_t index, u64 value) {
    impl->SetRegister(index, value);
}

std::array<u64, 31> Jit::GetRegisters() const {
    return impl->GetRegisters();
}

void Jit::SetRegisters(const std::array<u64, 31>& value) {
    impl->SetRegisters(value);
}

Vector Jit::GetVector(size_t index) const {
    return impl->GetVector(index);
}

void Jit::SetVector(size_t index, Vector value) {
    impl->SetVector(index, value);
}

std::array<Vector, 32> Jit::GetVectors() const {
    return impl->GetVectors();
}

void Jit::SetVectors(const std::array<Vector, 32>& value) {
    impl->SetVectors(value);
}

uint64_t * Jit::GetVectorData()
{
    return impl->GetVectorData();
}

u32 Jit::GetFpcr() const
{
    return impl->GetFpcr();
}

void Jit::SetFpcr(u32 value) {
    impl->SetFpcr(value);
}

u32 Jit::GetFpsr() const {
    return impl->GetFpsr();
}

void Jit::SetFpsr(u32 value) {
    impl->SetFpsr(value);
}

u32 Jit::GetPstate() const {
    return impl->GetPstate();
}

void Jit::SetPstate(u32 value) {
    impl->SetPstate(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

bool Jit::IsExecuting() const {
    return impl->IsExecuting();
}

void Jit::DumpDisassembly() const {
    return impl->DumpDisassembly();
}

std::vector<std::string> Jit::Disassemble() const {
    return impl->Disassemble();
}

}  // namespace Dynarmic::A64
