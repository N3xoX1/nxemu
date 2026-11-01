// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#if defined(__APPLE__)
#include <mach/mach_types.h>
#include <pthread.h>
#include <signal.h>
#include <vector>
#endif

#include "dynarmic/interface/halt_reason.h"
#include "nce/guest_context.h"
#include "yuzu_common/common_types.h"

namespace Core {

class ArmNce final : public ICpuCore {
public:
    ArmNce(ICoreSystem & system, bool uses_wall_clock, IKernelProcess & process, std::size_t core_index);
    ~ArmNce();

    void Initialize() override;

    ProcessorArchitecture GetArchitecture() const override
    {
        return ProcessorArchitecture::AArch64;
    }

    CpuHaltReason RunThread(IKernelThread * thread) override;
    CpuHaltReason StepThread(IKernelThread * thread) override;

    void GetContext(CpuThreadContext & ctx) const override;
    void SetContext(const CpuThreadContext & ctx) override;
    void SetTpidrroEl0(uint64_t value) override;

    void GetSvcArguments(uint64_t (&args)[8]) const override;
    void SetSvcArguments(const uint64_t (&args)[8]) override;
    uint32_t GetSvcNumber() const override;

    void SignalInterrupt(IKernelThread * thread) override;
    void ClearInstructionCache() override;
    void InvalidateCacheRange(uint64_t addr, uint64_t size) override;

    void LockThread(IKernelThread * thread) override;
    void UnlockThread(IKernelThread * thread) override;

    const CpuDebugWatchpoint * HaltedWatchpoint() const override
    {
        return nullptr;
    }

    void RewindBreakpointInstruction() override
    {
    }
    void SetWatchpointArray(const CpuDebugWatchpoint * /*watchpoints*/, uint32_t /*count*/) override
    {
    }

    void Release() override;

private:
    // Assembly definitions.
    static Dynarmic::HaltReason ReturnToRunCodeByTrampoline(void * tpidr, GuestContext * ctx, uint64_t trampoline_addr);
#if defined(__APPLE__)
    static Dynarmic::HaltReason ReturnToRunCodeByExceptionLevelChange(mach_port_t thread, GuestContext * ctx);

    static void LockThreadParameters(void * tpidr);
    static void UnlockThreadParameters(void * tpidr);

    static void EnterGuestSignalHandler(int sig, siginfo_t * info, void * raw_context);
    static void ExitGuestSignalHandler(int sig, siginfo_t * info, void * raw_context);
    static bool HandleGuestFault(ArmNce & nce, int sig, siginfo_t * info, void * raw_context);
    static void SaveGuestContext(GuestContext * ctx, void * raw_context);
    bool IsGuestAddress(uint64_t addr) const;

public:
    ICoreSystem & m_system;

    std::size_t m_core_index{};
    pthread_t m_thread{};
    mach_port_t m_thread_port{};

    GuestContext m_guest_ctx{};
    IKernelThread * m_running_thread{};

    // A break signal was sent and not yet received. It stays blocked while the core handles
    // a guest fault, which may wait on the GPU, so later interrupts must not wait for it.
    std::atomic<bool> m_break_pending{};

    // Ranges of the host address space that hold guest memory; guest code only runs there.
    std::vector<std::pair<uint64_t, uint64_t>> m_guest_windows;

    std::unique_ptr<u8[]> m_stack{};
};
#else
    static Dynarmic::HaltReason ReturnToRunCodeByExceptionLevelChange(int tid, void * tpidr);

    static void ReturnToRunCodeByExceptionLevelChangeSignalHandler(int sig, void * info, void * raw_context);
    static void BreakFromRunCodeSignalHandler(int sig, void * info, void * raw_context);
    static void GuestAlignmentFaultSignalHandler(int sig, void * info, void * raw_context);
    static void GuestAccessFaultSignalHandler(int sig, void * info, void * raw_context);

    static void LockThreadParameters(void * tpidr);
    static void UnlockThreadParameters(void * tpidr);

private:
    static void * RestoreGuestContext(void * raw_context);
    static void SaveGuestContext(GuestContext * ctx, void * raw_context);
    static bool HandleFailedGuestFault(GuestContext * ctx, void * info, void * raw_context);
    static bool HandleGuestAlignmentFault(GuestContext * ctx, void * info, void * raw_context);
    static bool HandleGuestAccessFault(GuestContext * ctx, void * info, void * raw_context);
    static void HandleHostAlignmentFault(int sig, void * info, void * raw_context);
    static void HandleHostAccessFault(int sig, void * info, void * raw_context);

public:
    ICoreSystem & m_system;

    std::size_t m_core_index{};
    pid_t m_thread_id{-1};

    GuestContext m_guest_ctx{};
    IKernelThread * m_running_thread{};

    std::unique_ptr<u8[]> m_stack{};
};
#endif

} // namespace Core
