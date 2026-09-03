// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "nxemu-os/core/hardware_properties.h"
#include <dynarmic/interface/A32/a32.h>
#include <nxemu-module-spec/base.h>
#include <nxemu-module-spec/cpu.h>
#include <yuzu_common/hardware_properties.h>

nxinterface IExclusiveMonitor;
class DynarmicCallbacks32;
namespace Core
{
class DynarmicCP15;
}

class ArmDynarmic32 final : 
    public ICpuCore
{
public:
    ArmDynarmic32(ICoreSystem & system, bool uses_wall_clock, IKernelProcess & process, Dynarmic::ExclusiveMonitor & monitor, uint32_t core_index);
    ~ArmDynarmic32();

    bool IsInThumbMode() const;

    // ICpuCore
    void Initialize() override;
    ProcessorArchitecture GetArchitecture() const override;
    uint32_t GetSvcNumber() const override;
    void GetContext(CpuThreadContext & ctx) const override;
    void SetContext(const CpuThreadContext & ctx) override;
    void GetSvcArguments(uint64_t (&args)[8]) const override;
    void SetSvcArguments(const uint64_t (&args)[8]) override;
    void SetTpidrroEl0(uint64_t value) override;
    CpuHaltReason RunThread(IKernelThread * thread) override;
    CpuHaltReason StepThread(IKernelThread * thread) override;
    void LockThread(IKernelThread * thread) override;
    void UnlockThread(IKernelThread * thread) override;
    void ClearInstructionCache() override;
    void InvalidateCacheRange(uint64_t addr, uint64_t size) override;
    const CpuDebugWatchpoint * HaltedWatchpoint() const override;
    void RewindBreakpointInstruction() override;
    void SetWatchpointArray(const CpuDebugWatchpoint * watchpoints, uint32_t count) override;
    void SignalInterrupt(IKernelThread * thread) override;

    void Release() override;

private:
    ICoreSystem & m_system;
    Dynarmic::ExclusiveMonitor & m_monitor;

private:
    friend class DynarmicCallbacks32;
    friend class Core::DynarmicCP15;
        
    std::shared_ptr<Dynarmic::A32::Jit> MakeJit(IKernelProcess & process) const;

    std::unique_ptr<DynarmicCallbacks32> m_cb{};
    std::shared_ptr<Core::DynarmicCP15> m_cp15{};
    uint32_t m_core_index{};
    bool m_uses_wall_clock;

    std::shared_ptr<Dynarmic::A32::Jit> m_jit{};

    // SVC callback
    u32 m_svc_swi{};

    // Watchpoint info
    const CpuDebugWatchpoint * m_halted_watchpoint{};
    CpuThreadContext m_breakpoint_context{};
    std::array<CpuDebugWatchpoint, Hardware::NUM_WATCHPOINTS> m_watchpoints;
};
