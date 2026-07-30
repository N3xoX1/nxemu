// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu_settings.h"
#include <yuzu_common/logging/log.h>
#include "arm_dynarmic.h"
#include "arm_dynarmic_64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include <common/maths.h>
#include <yuzu_common/page_table.h>
#include <yuzu_common/hardware_properties.h>
#include "yuzu_common/literals.h"

extern IModuleNotification * g_notify;

using Vector = Dynarmic::A64::Vector;
using namespace Common::Literals;

class DynarmicCallbacks64 :
    public Dynarmic::A64::UserCallbacks
{
public:
    explicit DynarmicCallbacks64(ArmDynarmic64 & parent, ICoreSystem & system, IKernelProcess & process) :
        m_parent{parent}, 
        m_memory(process.GetMemory()),
        m_system(system),
        m_process(process),
        m_debugger_enabled{system.DebuggerEnabled()},
        m_check_memory_access{m_debugger_enabled || !cpuSettings.cpuopt_ignore_memory_aborts}
    {
    }

    uint8_t MemoryRead8(uint64_t vaddr) override
    {
        if (m_check_memory_access)
        {
            CheckMemoryAccess(vaddr, 1, CpuDebugWatchpointType::Read);        
        }
        return m_memory.Read8(vaddr);
    }
    uint16_t MemoryRead16(uint64_t vaddr) override
    {
        if (m_check_memory_access)
        {
            CheckMemoryAccess(vaddr, 2, CpuDebugWatchpointType::Read);
        }
        return m_memory.Read16(vaddr);
    }
    uint32_t MemoryRead32(uint64_t vaddr) override
    {
        if (m_check_memory_access)
        {
            CheckMemoryAccess(vaddr, 4, CpuDebugWatchpointType::Read);
        }
        return m_memory.Read32(vaddr);
    }
    uint64_t MemoryRead64(uint64_t vaddr) override
    {
        if (m_check_memory_access)
        {
            CheckMemoryAccess(vaddr, 8, CpuDebugWatchpointType::Read);
        }
        return m_memory.Read64(vaddr);
    }
    Vector MemoryRead128(uint64_t vaddr) override
    {
        if (m_check_memory_access)
        {
            CheckMemoryAccess(vaddr, 16, CpuDebugWatchpointType::Read);
        }
        return {m_memory.Read64(vaddr), m_memory.Read64(vaddr + 8)};
    }

    void MemoryWrite8(uint64_t vaddr, uint8_t value) override
    {
        if (!m_check_memory_access || CheckMemoryAccess(vaddr, 1, CpuDebugWatchpointType::Write))
        {
            m_memory.Write8(vaddr, value);
        }
    }

    void MemoryWrite16(uint64_t vaddr, uint16_t value) override
    {
        if (!m_check_memory_access || CheckMemoryAccess(vaddr, 2, CpuDebugWatchpointType::Write))
        {
            m_memory.Write16(vaddr, value);
        }
    }

    void MemoryWrite32(uint64_t vaddr, uint32_t value) override
    {
        if (!m_check_memory_access || CheckMemoryAccess(vaddr, 4, CpuDebugWatchpointType::Write))
        {
            m_memory.Write32(vaddr, value);
        }
    }
    void MemoryWrite64(uint64_t vaddr, uint64_t value) override
    {
        if (!m_check_memory_access || CheckMemoryAccess(vaddr, 8, CpuDebugWatchpointType::Write))
        {
            m_memory.Write64(vaddr, value);
        }
    }
    void MemoryWrite128(uint64_t vaddr, Vector value) override
    {
        if (!m_check_memory_access || CheckMemoryAccess(vaddr, 16, CpuDebugWatchpointType::Write))
        {
            m_memory.Write64(vaddr, value[0]);
            m_memory.Write64(vaddr + 8, value[1]);
        }
    }

    bool MemoryWriteExclusive8(u64 vaddr, std::uint8_t value, std::uint8_t expected) override
    {
        return CheckMemoryAccess(vaddr, 1, CpuDebugWatchpointType::Write) && m_memory.WriteExclusive8(vaddr, value, expected);
    }
    bool MemoryWriteExclusive16(u64 vaddr, std::uint16_t value, std::uint16_t expected) override
    {
        return CheckMemoryAccess(vaddr, 2, CpuDebugWatchpointType::Write) && m_memory.WriteExclusive16(vaddr, value, expected);
    }
    bool MemoryWriteExclusive32(u64 vaddr, std::uint32_t value, std::uint32_t expected) override
    {
        return CheckMemoryAccess(vaddr, 4, CpuDebugWatchpointType::Write) && m_memory.WriteExclusive32(vaddr, value, expected);
    }
    bool MemoryWriteExclusive64(u64 vaddr, std::uint64_t value, std::uint64_t expected) override
    {
        return CheckMemoryAccess(vaddr, 8, CpuDebugWatchpointType::Write) && m_memory.WriteExclusive64(vaddr, value, expected);
    }
    bool MemoryWriteExclusive128(uint64_t vaddr, Vector value, Vector expected) override
    {
        return CheckMemoryAccess(vaddr, 16, CpuDebugWatchpointType::Write) && m_memory.WriteExclusive128(vaddr, value[1], value[0], expected[1], expected[0]);
    }

    void InterpreterFallback(uint64_t /*pc*/, uint64_t /*num_instructions*/) override
    {
        g_notify->BreakPoint(__FILE__, __LINE__);
    }

    void InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, uint64_t value) override
    {
        switch (op)
        {
        case Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU:
        {
            static constexpr u64 ICACHE_LINE_SIZE = 64;

            const u64 cache_line_start = value & ~(ICACHE_LINE_SIZE - 1);
            m_parent.InvalidateCacheRange(cache_line_start, ICACHE_LINE_SIZE);
            break;
        }
        case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoU:
            m_parent.ClearInstructionCache();
            break;
        case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoUInnerSharable:
        default:
            LOG_DEBUG(Core_ARM, "Unprocesseed instruction cache operation: {}", op);
            break;
        }

        m_parent.m_jit->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
    }

    void ExceptionRaised(uint64_t pc, Dynarmic::A64::Exception exception) override
    {
        switch (exception)
        {
        case Dynarmic::A64::Exception::WaitForInterrupt:
        case Dynarmic::A64::Exception::WaitForEvent:
        case Dynarmic::A64::Exception::SendEvent:
        case Dynarmic::A64::Exception::SendEventLocal:
        case Dynarmic::A64::Exception::Yield:
            return;
        case Dynarmic::A64::Exception::NoExecuteFault:
            LOG_CRITICAL(Core_ARM, "Cannot execute instruction at unmapped address {:#016x}", pc);
            g_notify->BreakPoint(__FILE__, __LINE__);
            return;
        default:
            if (m_debugger_enabled)
            {
                g_notify->BreakPoint(__FILE__, __LINE__);
            }
            else
            {
                m_process.LogBacktrace(m_parent);
                LOG_CRITICAL(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X})", static_cast<std::size_t>(exception), pc, m_memory.Read32(pc));
            }
        }
    }

    void CallSVC(uint32_t svc) override
    {
        m_parent.m_svc = svc;
        m_parent.m_jit->HaltExecution(TranslateDynarmicHaltReason(CpuHaltReason::SupervisorCall));
    }

    void AddTicks(uint64_t /*ticks*/) override
    {
        g_notify->BreakPoint(__FILE__, __LINE__);
    }

    uint64_t GetTicksRemaining() override
    {
        g_notify->BreakPoint(__FILE__, __LINE__);
        return 0;
    }

    uint64_t GetCNTPCT() override
    {
        return m_parent.m_system.Timing().GetClockTicks();
    }

    bool CheckMemoryAccess(uint64_t /*addr*/, uint64_t /*size*/, CpuDebugWatchpointType /*type*/)
    {
        if (!m_check_memory_access)
        {
            return true;
        }
        g_notify->BreakPoint(__FILE__, __LINE__);
        return true;
    }

    ArmDynarmic64 & m_parent;
    IMemory & m_memory;
    ICoreSystem & m_system;
    IKernelProcess & m_process;
    uint64_t m_tpidrro_el0{};
    uint64_t m_tpidr_el0{};
    const bool m_debugger_enabled{};
    const bool m_check_memory_access{};
};

ArmDynarmic64::ArmDynarmic64(ICoreSystem & system, bool uses_wall_clock, IKernelProcess & process, Dynarmic::ExclusiveMonitor & monitor, uint32_t core_index) :
    m_jit(nullptr),
    m_cb(std::make_unique<DynarmicCallbacks64>(*this, system, process)),
    m_uses_wall_clock(uses_wall_clock),
    m_system(system),
    m_monitor(monitor),
    m_svc(0),
    m_process(process),
    m_coreIndex(core_index)
{
    m_jit = MakeJit(process);
    ScopedJitExecution::RegisterHandler();
}

ArmDynarmic64::~ArmDynarmic64()
{
}

std::shared_ptr<Dynarmic::A64::Jit> ArmDynarmic64::MakeJit(IKernelProcess & process) const
{
    IKProcessPageTable & page_table = process.GetPageTable();

    Dynarmic::A64::UserConfig config;

    // Callbacks
    config.callbacks = m_cb.get();

    // Memory
    config.page_table = page_table.PageTable();
    config.page_table_address_space_bits = page_table.GetAddressSpaceWidth();
    config.page_table_pointer_mask_bits = Common::PageTable::ATTRIBUTE_BITS;
    config.silently_mirror_page_table = false;
    config.absolute_offset_page_table = true;
    config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    config.only_detect_misalignment_via_page_table_on_page_boundary = true;

    config.fastmem_pointer = page_table.FastmemArena();
    config.fastmem_address_space_bits = page_table.GetAddressSpaceWidth();
    config.silently_mirror_fastmem = false;

    config.fastmem_exclusive_access = config.fastmem_pointer != nullptr;
    config.recompile_on_exclusive_fastmem_failure = true;

    // Multi-process state
    config.processor_id = m_coreIndex;
    config.global_monitor = &m_monitor;

    // System registers
    config.tpidrro_el0 = &m_cb->m_tpidrro_el0;
    config.tpidr_el0 = &m_cb->m_tpidr_el0;
    config.dczid_el0 = 4;
    config.ctr_el0 = 0x8444c004;
    config.cntfrq_el0 = Hardware::CNTFREQ;

    // Unpredictable instructions
    config.define_unpredictable_behaviour = true;

    // Timing
    config.wall_clock_cntpct = m_uses_wall_clock;
    config.enable_cycle_counting = !m_uses_wall_clock;

    // Code cache size
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64)
    config.code_cache_size = 128_MiB;
#else
    config.code_cache_size = 512_MiB;
#endif

    // Allow memory fault handling to work
    if (m_system.DebuggerEnabled())
    {
        config.check_halt_on_memory_access = true;
    }

    // Safe optimizations
    if (cpuSettings.cpu_debug_mode)
    {
        if (!cpuSettings.cpuopt_page_tables)
        {
            config.page_table = nullptr;
        }
        if (!cpuSettings.cpuopt_block_linking)
        {
            config.optimizations &= ~Dynarmic::OptimizationFlag::BlockLinking;
        }
        if (!cpuSettings.cpuopt_return_stack_buffer)
        {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
        }
        if (!cpuSettings.cpuopt_fast_dispatcher)
        {
            config.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
        }
        if (!cpuSettings.cpuopt_context_elimination)
        {
            config.optimizations &= ~Dynarmic::OptimizationFlag::GetSetElimination;
        }
        if (!cpuSettings.cpuopt_const_prop)
        {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ConstProp;
        }
        if (!cpuSettings.cpuopt_misc_ir)
        {
            config.optimizations &= ~Dynarmic::OptimizationFlag::MiscIROpt;
        }
        if (!cpuSettings.cpuopt_reduce_misalign_checks)
        {
            config.only_detect_misalignment_via_page_table_on_page_boundary = false;
        }
        if (!cpuSettings.cpuopt_fastmem)
        {
            config.fastmem_pointer = nullptr;
            config.fastmem_exclusive_access = false;
        }
        if (!cpuSettings.cpuopt_fastmem_exclusives)
        {
            config.fastmem_exclusive_access = false;
        }
        if (!cpuSettings.cpuopt_recompile_exclusives)
        {
            config.recompile_on_exclusive_fastmem_failure = false;
        }
        if (!cpuSettings.cpuopt_ignore_memory_aborts)
        {
            config.check_halt_on_memory_access = true;
        }
    }
    else
    {
        // Unsafe optimizations
        if (cpuSettings.cpu_accuracy == CpuAccuracy::Unsafe)
        {
            config.unsafe_optimizations = true;
            if (cpuSettings.cpuopt_unsafe_unfuse_fma)
            {
                config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
            }
            if (cpuSettings.cpuopt_unsafe_reduce_fp_error)
            {
                config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
            }
            if (cpuSettings.cpuopt_unsafe_inaccurate_nan)
            {
                config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
            }
            if (cpuSettings.cpuopt_unsafe_fastmem_check)
            {
                config.fastmem_address_space_bits = 64;
            }
            if (cpuSettings.cpuopt_unsafe_ignore_global_monitor)
            {
                config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
            }
        }

        // Curated optimizations
        if (cpuSettings.cpu_accuracy == CpuAccuracy::Auto)
        {
            config.unsafe_optimizations = true;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
            config.fastmem_address_space_bits = 64;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }

        // Paranoia mode for debugging optimizations
        if (cpuSettings.cpu_accuracy == CpuAccuracy::Paranoid)
        {
            config.unsafe_optimizations = false;
            config.optimizations = Dynarmic::no_optimizations;
        }
    }

    return std::make_shared<Dynarmic::A64::Jit>(config);
}

void ArmDynarmic64::Initialize()
{
}

ProcessorArchitecture ArmDynarmic64::GetArchitecture() const
{
    return ProcessorArchitecture::AArch64;
}

CpuHaltReason ArmDynarmic64::RunThread(IKernelThread * thread)
{
    ScopedJitExecution sj(thread->GetOwnerProcess());

    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Run());
}

CpuHaltReason ArmDynarmic64::StepThread(IKernelThread * thread)
{
    ScopedJitExecution sj(thread->GetOwnerProcess());

    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Step());
}

void ArmDynarmic64::LockThread(IKernelThread * /*thread*/)
{
}

void ArmDynarmic64::UnlockThread(IKernelThread * /*thread*/)
{
}

uint32_t ArmDynarmic64::GetSvcNumber() const
{
    return m_svc;
}

void ArmDynarmic64::GetSvcArguments(uint64_t (&args)[8]) const
{
    Dynarmic::A64::Jit & j = *m_jit;
    for (size_t i = 0, n = sizeof(args) / sizeof(args[0]); i < n; i++)
    {
        args[i] = j.GetRegister(i);
    }
}

void ArmDynarmic64::SetSvcArguments(const uint64_t (&args)[8])
{
    Dynarmic::A64::Jit & j = *m_jit;
    for (size_t i = 0, n = sizeof(args) / sizeof(args[0]); i < n; i++)
    {
        j.SetRegister(i, args[i]);
    }
}

const CpuDebugWatchpoint * ArmDynarmic64::HaltedWatchpoint() const
{
    return m_halted_watchpoint;
}

void ArmDynarmic64::RewindBreakpointInstruction()
{
    this->SetContext(m_breakpoint_context);
}

void ArmDynarmic64::SetTpidrroEl0(u64 value)
{
    m_cb->m_tpidrro_el0 = value;
}

void ArmDynarmic64::GetContext(CpuThreadContext & ctx) const
{
    Dynarmic::A64::Jit& j = *m_jit;
    auto gpr = j.GetRegisters();

    // TODO: this is inconvenient
    for (size_t i = 0; i < 29; i++)
    {
        ctx.r[i] = gpr[i];
    }
    ctx.fp = gpr[29];
    ctx.lr = gpr[30];

    ctx.sp = j.GetSP();
    ctx.pc = j.GetPC();
    ctx.pstate = j.GetPstate();
    memcpy(ctx.v, j.GetVectorData(), sizeof(ctx.v));
    ctx.fpcr = j.GetFpcr();
    ctx.fpsr = j.GetFpsr();
    ctx.tpidr = m_cb->m_tpidr_el0;
}

void ArmDynarmic64::SetContext(const CpuThreadContext & ctx)
{
    Dynarmic::A64::Jit& j = *m_jit;

    // TODO: this is inconvenient
    std::array<u64, 31> gpr;

    for (size_t i = 0; i < 29; i++)
    {
        gpr[i] = ctx.r[i];
    }
    gpr[29] = ctx.fp;
    gpr[30] = ctx.lr;

    j.SetRegisters(gpr);
    j.SetSP(ctx.sp);
    j.SetPC(ctx.pc);
    j.SetPstate(ctx.pstate);
    memcpy(j.GetVectorData(), ctx.v, sizeof(ctx.v));
    j.SetFpcr(ctx.fpcr);
    j.SetFpsr(ctx.fpsr);
    m_cb->m_tpidr_el0 = ctx.tpidr;
}

void ArmDynarmic64::SetWatchpointArray(const CpuDebugWatchpoint * watchpoints, uint32_t count)
{
    for (uint32_t i = 0, n = count < (uint32_t)m_watchpoints.size() ? count : (uint32_t)m_watchpoints.size(); i < n; i++)
    {
        m_watchpoints[i] = watchpoints[i];
    }
}

void ArmDynarmic64::SignalInterrupt(IKernelThread * /*thread*/)
{
    m_jit->HaltExecution(TranslateDynarmicHaltReason(CpuHaltReason::BreakLoop));
}

void ArmDynarmic64::ClearInstructionCache()
{
    m_jit->ClearCache();
}

void ArmDynarmic64::InvalidateCacheRange(uint64_t addr, uint64_t size)
{
    m_jit->InvalidateCacheRange(addr, size);
}

void ArmDynarmic64::Release()
{
    delete this;
}
