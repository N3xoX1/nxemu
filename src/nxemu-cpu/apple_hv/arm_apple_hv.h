// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Architecture based in part on Ryubing/Ryujinx AppleHv (MIT):
// Copyright (c) Ryujinx Team and Contributors.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>

#include <nxemu-module-spec/cpu.h>
#include "yuzu_common/common_types.h"

namespace Core::AppleHv {
class AddressSpace;
}

namespace Core {

class ArmAppleHv final : public ICpuCore {
public:
    ArmAppleHv(ICoreSystem& system, IKernelProcess& process,
               std::shared_ptr<AppleHv::AddressSpace> address_space, std::size_t core_index);
    ~ArmAppleHv();

    void Initialize() override {}
    ProcessorArchitecture GetArchitecture() const override { return ProcessorArchitecture::AArch64; }
    u32 GetSvcNumber() const override;
    void GetContext(CpuThreadContext& ctx) const override;
    void SetContext(const CpuThreadContext& ctx) override;
    void GetSvcArguments(u64 (&args)[8]) const override;
    void SetSvcArguments(const u64 (&args)[8]) override;
    void SetTpidrroEl0(u64 value) override;
    CpuHaltReason RunThread(IKernelThread* thread) override;
    CpuHaltReason StepThread(IKernelThread* thread) override;
    void LockThread(IKernelThread*) override {}
    void UnlockThread(IKernelThread*) override {}
    void ClearInstructionCache() override;
    void InvalidateCacheRange(u64 addr, u64 size) override;
    const CpuDebugWatchpoint* HaltedWatchpoint() const override;
    void RewindBreakpointInstruction() override;
    void SetWatchpointArray(const CpuDebugWatchpoint* watchpoints, u32 count) override;
    void SignalInterrupt(IKernelThread* thread) override;
    void Release() override { delete this; }

private:
    CpuHaltReason RunInternal(bool single_step);
    std::optional<CpuHaltReason> HandleSynchronousException();
    const CpuDebugWatchpoint* MatchingWatchpoint(u64 addr, u64 size,
                                                 CpuDebugWatchpointType access_type) const;

    u64 MaintenanceAddress();
    bool PrepareResume();
    bool CreateVcpu();
    bool ArmVTimer();
    void DestroyVcpu();
    bool LoadContextToVcpu();
    bool SaveCurrentEl0Context();
    bool SaveExceptionEl0Context(u64 resume_pc);
    bool HandleTrappedSystemRegister(u64 esr);
    bool HvSucceeded(u32 result, const char* operation) const;

private:
    ICoreSystem& m_system;
    IKernelProcess& m_process;
    std::shared_ptr<AppleHv::AddressSpace> m_address_space;
    std::size_t m_core_index{};

    CpuThreadContext m_context{};
    u64 m_tpidrro_el0{};
    u32 m_svc_number{};
    const CpuDebugWatchpoint* m_watchpoints{};
    u32 m_watchpoint_count{};
    const CpuDebugWatchpoint* m_halted_watchpoint{};

    static constexpr u64 InvalidVcpu = ~u64{0};
    std::atomic<u64> m_published_vcpu{InvalidVcpu};
    std::atomic_bool m_interrupt_requested{};
    std::atomic_bool m_running{};
    mutable std::mutex m_vcpu_lifetime_mutex;

    u64 m_vcpu{InvalidVcpu};
    void* m_exit_info{};
    u64 m_vcpu_generation{};
    u64 m_vcpu_code_generation{};
    bool m_logged_run{};
    bool m_code_flush_pending{};
    u32 m_vtimer_continue_count{};
};

} // namespace Core
