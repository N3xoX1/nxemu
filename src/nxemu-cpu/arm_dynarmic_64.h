// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "dynarmic/interface/A64/a64.h"
#include <yuzu_common/hardware_properties.h>
#include <nxemu-module-spec/cpu.h>

class DynarmicCallbacks64;

class ArmDynarmic64 final :
    public ICpuCore
{
    friend class DynarmicCallbacks64;

public:
    ArmDynarmic64(ICoreSystem & system, bool uses_wall_clock, IKernelProcess & process, Dynarmic::ExclusiveMonitor & monitor, uint32_t core_index);
    ~ArmDynarmic64();

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
    ArmDynarmic64() = delete;
    ArmDynarmic64(const ArmDynarmic64 &) = delete;
    ArmDynarmic64 & operator=(const ArmDynarmic64 &) = delete;

    std::shared_ptr<Dynarmic::A64::Jit> MakeJit(IKernelProcess & process) const;

    std::shared_ptr<Dynarmic::A64::Jit> m_jit{};
    std::unique_ptr<DynarmicCallbacks64> m_cb{};
    bool m_uses_wall_clock;

    // Watchpoint info
    const CpuDebugWatchpoint * m_halted_watchpoint{};
    CpuThreadContext m_breakpoint_context{};
    std::array<CpuDebugWatchpoint, Hardware::NUM_WATCHPOINTS> m_watchpoints;
    ICoreSystem & m_system;
    Dynarmic::ExclusiveMonitor & m_monitor;
    uint32_t m_svc;
    IKernelProcess & m_process;
    uint32_t m_coreIndex;
};
