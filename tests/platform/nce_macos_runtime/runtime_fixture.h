#pragma once
// SPDX-License-Identifier: GPL-2.0-or-later

#include "nce/arm_nce.h"
#include "nce/patcher.h"
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace NceTest {
constexpr uint64_t Base = 0x7000000000, Size = 0x1000000;
inline void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::abort(); }
}
struct Timing final : ICoreTiming {
    void AddTicks(uint64_t) override {}
    int64_t GetDowncount() const override { return 1000000; }
    uint64_t GetClockTicks() const override { return 0; }
    void BeginCpuRun(uint32_t) override {}
    void EndCpuRun(uint32_t) override {}
    bool CpuHleSynchronizationEnabled() const override { return false; }
    void CpuCompilation(uint32_t, bool) override {}
};
struct System final : ICoreSystem {
    NceTest::Timing timing;
    bool DebuggerEnabled() const override { return false; }
    ICoreTiming& Timing() override { return timing; }
};
struct Memory final : IMemory {
    bool IsValidVirtualAddressRange(uint64_t a, uint64_t n) const override { return a >= Base && a - Base + n <= Size; }
    void RasterizerMarkRegionCached(uint64_t, uint64_t, bool) override {}
    enum class FaultMode { Reject, Emulate, OpenOnWrite };
    FaultMode fault_mode{FaultMode::Reject};
    uint64_t redirect_begin{}, redirect_size{};
    uint8_t* redirect{};
    std::atomic<uint64_t> faults{};
    uint64_t* native_atomic_table{};
    uint8_t* GetPointerSilent(uint64_t a) override {
        return redirect && a >= redirect_begin && a - redirect_begin < redirect_size
            ? redirect + (a - redirect_begin) : reinterpret_cast<uint8_t*>(a);
    }
    bool InvalidateNCE(uint64_t, uint64_t) override {
        if (fault_mode == FaultMode::Emulate) ++faults;
        return false;
    }
    bool NceAccessAllowed(uint64_t, bool) override { return fault_mode != FaultMode::Reject; }
    bool NceTrackWrites(uint64_t a) override {
        if (fault_mode != FaultMode::OpenOnWrite) return false;
        ++faults;
        if (native_atomic_table) std::atomic_ref<uint64_t>(native_atomic_table[a >> 12]).fetch_or(2);
        return mprotect(reinterpret_cast<void*>(a & ~uint64_t(0x3fff)), 0x4000, PROT_READ | PROT_WRITE) == 0;
    }
#define ACCESS(bits) \
    uint##bits##_t Read##bits(uint64_t a) override { return *reinterpret_cast<uint##bits##_t*>(GetPointerSilent(a)); } \
    void Write##bits(uint64_t a, uint##bits##_t v) override { *reinterpret_cast<uint##bits##_t*>(GetPointerSilent(a)) = v; } \
    bool WriteExclusive##bits(uint64_t, uint##bits##_t, uint##bits##_t) override { std::abort(); }
    ACCESS(8) ACCESS(16) ACCESS(32) ACCESS(64)
#undef ACCESS
    bool ReadBlock(uint64_t a, void* p, uint64_t n) override { std::memcpy(p, GetPointerSilent(a), n); return true; }
    bool WriteBlock(uint64_t a, const void* p, uint64_t n) override { std::memcpy(GetPointerSilent(a), p, n); return true; }
    bool WriteExclusive128(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) override { std::abort(); }
};
struct TestPageTable final : IKProcessPageTable {
    uint64_t GetNceAtomicTable() const override { return 0; }
    uint32_t GetAddressSpaceWidth() const override { return 39; }
    uint8_t* FastmemArena() const override { return nullptr; }
    void** PageTable() const override { return nullptr; }
    uint32_t GetNceWindows(uint64_t* starts, uint64_t* ends, uint32_t max) const override {
        if (!max) return 0;
        starts[0] = Base; ends[0] = Base + Size; return 1;
    }
};
struct Process final : IKernelProcess {
    NceTest::Memory memory;
    TestPageTable pages;
    Core::NCE::EntryTrampolines handlers;
    Core::NCE::RestartPcs restarts;
    Core::NCE::EntryTrampolines atomics;
    bool force_signal_entry{};
    std::atomic<bool> atomics_enabled{};
    IKProcessPageTable& GetPageTable() override { return pages; }
    IMemory& GetMemory() override { return memory; }
    bool Is64Bit() const override { return true; }
    bool IsApplication() const override { return true; }
    uint64_t FindPostHandler(uint64_t pc) const override {
        if (force_signal_entry) return 0;
        const auto it = handlers.find(pc); return it == handlers.end() ? 0 : it->second;
    }
    void RegisterPostHandler(uint64_t a, uint64_t b) override { handlers.emplace(a, b); }
    void LogBacktrace(ICpuCore&) override {}
    uint64_t GetNceRestartPc(uint64_t pc) const override {
        auto it = restarts.find(pc); return it == restarts.end() ? pc : it->second;
    }
    void RegisterNceRestartPc(uint64_t pc, uint64_t restart) override { restarts.emplace(pc, restart); }
    uint64_t FindNceAtomicHandler(uint64_t pc) const override {
        const auto it = atomics.find(pc); return it == atomics.end() ? 0 : it->second;
    }
    void RegisterNceAtomicHandler(uint64_t pc, uint64_t handler) override { atomics.emplace(pc, handler); }
    void EnableNceAtomicHandlers() override {
        if (atomics_enabled.exchange(true)) return;
        for (const auto& [pc, target] : atomics) {
            auto* word = reinterpret_cast<uint32_t*>(memory.GetPointerSilent(pc));
            const auto distance = static_cast<int64_t>(target) - static_cast<int64_t>(pc);
            std::atomic_ref<uint32_t>(*word).store(0x14000000u | ((distance >> 2) & 0x03ffffffu));
            sys_dcache_flush(word, 4);
            sys_icache_invalidate(reinterpret_cast<void*>(pc), 4);
        }
    }
};
struct Thread final : IKernelThread {
    Process* process{};
    NativeExecutionParameters params;
    CpuThreadContext context{};
    uint64_t svc_count{}, stack_top{};
    std::mutex mutex;
    IKernelProcess* GetOwnerProcess() const override { return process; }
    NativeExecutionParameters& GetNativeExecutionParameters() override { return params; }
};
struct CoreState {
    std::mutex guard;
    Core::ArmNce cpu;
    Thread* current{};
    CoreState(System& s, Process& p, size_t i) : cpu(s, false, p, i) {}
};

} // namespace NceTest
