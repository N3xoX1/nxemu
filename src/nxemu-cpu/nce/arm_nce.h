// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>

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

} // namespace Core
