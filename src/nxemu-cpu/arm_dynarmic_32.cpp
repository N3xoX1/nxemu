// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu_settings.h"
#include "yuzu_common/literals.h"
#include "arm_dynarmic.h"
#include "arm_dynarmic_32.h"
#include "dynarmic_cp15.h"
#include "exclusive_monitor_interface.h"
#include <yuzu_common/page_table.h>
#include <yuzu_common/logging/log.h>
#include <yuzu_common/yuzu_assert.h>

using namespace Common::Literals;

class DynarmicCallbacks32 : public Dynarmic::A32::UserCallbacks
{
public:
    explicit DynarmicCallbacks32(ArmDynarmic32 & parent, IKernelProcess & process) :
        m_parent{parent}, 
        m_memory(process.GetMemory()),
        m_process(process),
        m_debugger_enabled{parent.m_system.DebuggerEnabled()},
        m_check_memory_access{m_debugger_enabled || !cpuSettings.cpuopt_ignore_memory_aborts}
    {
    }

    u8 MemoryRead8(u32 vaddr) override
    {
        CheckMemoryAccess(vaddr, 1, CpuDebugWatchpointType::Read);
        return m_memory.Read8(vaddr);
    }
    u16 MemoryRead16(u32 vaddr) override
    {
        CheckMemoryAccess(vaddr, 2, CpuDebugWatchpointType::Read);
        return m_memory.Read16(vaddr);
    }
    u32 MemoryRead32(u32 vaddr) override
    {
        CheckMemoryAccess(vaddr, 4, CpuDebugWatchpointType::Read);
        return m_memory.Read32(vaddr);
    }
    u64 MemoryRead64(u32 vaddr) override
    {
        CheckMemoryAccess(vaddr, 8, CpuDebugWatchpointType::Read);
        return m_memory.Read64(vaddr);
    }
    std::optional<u32> MemoryReadCode(u32 vaddr) override
    {
        if (!m_memory.IsValidVirtualAddressRange(vaddr, sizeof(u32)))
        {
            return std::nullopt;
        }
        return m_memory.Read32(vaddr);
    }

    void MemoryWrite8(u32 vaddr, u8 value) override
    {
        if (CheckMemoryAccess(vaddr, 1, CpuDebugWatchpointType::Write))
        {
            m_memory.Write8(vaddr, value);
        }
    }
    void MemoryWrite16(u32 vaddr, u16 value) override
    {
        if (CheckMemoryAccess(vaddr, 2, CpuDebugWatchpointType::Write))
        {
            m_memory.Write16(vaddr, value);
        }
    }
    void MemoryWrite32(u32 vaddr, u32 value) override
    {
        if (CheckMemoryAccess(vaddr, 4, CpuDebugWatchpointType::Write))
        {
            m_memory.Write32(vaddr, value);
        }
    }
    void MemoryWrite64(u32 vaddr, u64 value) override
    {
        if (CheckMemoryAccess(vaddr, 8, CpuDebugWatchpointType::Write))
        {
            m_memory.Write64(vaddr, value);
        }
    }

    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) override
    {
        return CheckMemoryAccess(vaddr, 1, CpuDebugWatchpointType::Write) && m_memory.WriteExclusive8(vaddr, value, expected);
    }
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) override
    {
        return CheckMemoryAccess(vaddr, 2, CpuDebugWatchpointType::Write) && m_memory.WriteExclusive16(vaddr, value, expected);
    }
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) override
    {
        return CheckMemoryAccess(vaddr, 4, CpuDebugWatchpointType::Write) && m_memory.WriteExclusive32(vaddr, value, expected);
    }
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) override
    {
        return CheckMemoryAccess(vaddr, 8, CpuDebugWatchpointType::Write) && m_memory.WriteExclusive64(vaddr, value, expected);
    }

    void InterpreterFallback(u32 pc, std::size_t num_instructions) override
    {
        m_process.LogBacktrace(m_parent);
        LOG_ERROR(Core_ARM, "Unimplemented instruction @ 0x{:X} for {} instructions (instr = {:08X})", pc, num_instructions, m_memory.Read32(pc));
    }

    void ExceptionRaised(u32 pc, Dynarmic::A32::Exception exception) override
    {
        switch (exception)
        {
        case Dynarmic::A32::Exception::NoExecuteFault:
            LOG_CRITICAL(Core_ARM, "Cannot execute instruction at unmapped address {:#08x}", pc);
            ReturnException(pc, TranslateDynarmicHaltReason(CpuHaltReason::PrefetchAbort));
            return;
        default:
            if (m_debugger_enabled)
            {
                ReturnException(pc, TranslateDynarmicHaltReason(CpuHaltReason::InstructionBreakpoint));
                return;
            }

            m_process.LogBacktrace(m_parent);
            LOG_CRITICAL(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X}, thumb = {})", exception, pc, m_memory.Read32(pc), m_parent.IsInThumbMode());
        }
    }

    void CallSVC(u32 swi) override
    {
        m_parent.m_svc_swi = swi;
        m_parent.m_jit->HaltExecution(TranslateDynarmicHaltReason(CpuHaltReason::SupervisorCall));
    }

    void AddTicks(u64 ticks) override
    {
        ASSERT_MSG(!m_parent.m_uses_wall_clock, "Dynarmic ticking disabled");

        // Divide the number of ticks by the amount of CPU cores. TODO(Subv): This yields only a
        // rough approximation of the amount of executed ticks in the system, it may be thrown off
        // if not all cores are doing a similar amount of work. Instead of doing this, we should
        // device a way so that timing is consistent across all cores without increasing the ticks 4
        // times.
        u64 amortized_ticks = ticks / Hardware::NUM_CPU_CORES;
        // Always execute at least one tick.
        amortized_ticks = std::max<u64>(amortized_ticks, 1);

        m_parent.m_system.Timing().AddTicks(amortized_ticks);
    }

    u64 GetTicksRemaining() override
    {
        ASSERT_MSG(!m_parent.m_uses_wall_clock, "Dynarmic ticking disabled");

        return std::max<s64>(m_parent.m_system.Timing().GetDowncount(), 0);
    }

    bool CheckMemoryAccess(u64 addr, u64 size, CpuDebugWatchpointType type)
    {
        if (!m_check_memory_access)
        {
            return true;
        }

        if (!m_memory.IsValidVirtualAddressRange(addr, size))
        {
            LOG_CRITICAL(Core_ARM, "Stopping execution due to unmapped memory access at {:#x}", addr);
            m_parent.m_jit->HaltExecution(TranslateDynarmicHaltReason(CpuHaltReason::PrefetchAbort));
            return false;
        }

        if (!m_debugger_enabled)
        {
            return true;
        }

#ifdef tofix
        const auto match{m_parent.MatchingWatchpoint(addr, size, type) };
        if (match)
        {
            m_parent.m_halted_watchpoint = match;
            m_parent.m_jit->HaltExecution(Core::DataAbort);
            return false;
        }
#endif
        return true;
    }

    void ReturnException(u32 pc, Dynarmic::HaltReason hr)
    {
        m_parent.GetContext(m_parent.m_breakpoint_context);
        m_parent.m_breakpoint_context.pc = pc;
        m_parent.m_breakpoint_context.r[15] = pc;
        m_parent.m_jit->HaltExecution(hr);
    }

    ArmDynarmic32 & m_parent;
    IMemory & m_memory;
    IKernelProcess & m_process;
    const bool m_debugger_enabled{};
    const bool m_check_memory_access{};
    static constexpr u64 MinimumRunCycles = 10000U;
};

std::shared_ptr<Dynarmic::A32::Jit> ArmDynarmic32::MakeJit(IKernelProcess & process) const
{
    IKProcessPageTable & page_table = process.GetPageTable();

    Dynarmic::A32::UserConfig config;
    config.callbacks = m_cb.get();
    config.coprocessors[15] = m_cp15;
    config.define_unpredictable_behaviour = true;

    constexpr size_t PageBits = 12;
    constexpr size_t NumPageTableEntries = 1 << (32 - PageBits);

    config.page_table = reinterpret_cast<std::array<std::uint8_t *, NumPageTableEntries> *>(page_table.PageTable());
    config.absolute_offset_page_table = true;
    config.page_table_pointer_mask_bits = Common::PageTable::ATTRIBUTE_BITS;
    config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
    config.only_detect_misalignment_via_page_table_on_page_boundary = true;

    config.fastmem_pointer = page_table.FastmemArena();

    config.fastmem_exclusive_access = config.fastmem_pointer != nullptr;
    config.recompile_on_exclusive_fastmem_failure = true;

    // Multi-process state
    config.processor_id = m_core_index;
    config.global_monitor = &m_monitor;

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
            if (cpuSettings.cpuopt_unsafe_ignore_standard_fpcr)
            {
                config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
            }
            if (cpuSettings.cpuopt_unsafe_inaccurate_nan)
            {
                config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
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
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }

        // Paranoia mode for debugging optimizations
        if (cpuSettings.cpu_accuracy == CpuAccuracy::Paranoid)
        {
            config.unsafe_optimizations = false;
            config.optimizations = Dynarmic::no_optimizations;
        }
    }

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

static std::pair<u32, u32> FpscrToFpsrFpcr(u32 fpscr) {
    // FPSCR bits [31:27] are mapped to FPSR[31:27].
    // FPSCR bit [7] is mapped to FPSR[7].
    // FPSCR bits [4:0] are mapped to FPSR[4:0].
    const u32 nzcv = fpscr & 0xf8000000;
    const u32 idc = fpscr & 0x80;
    const u32 fiq = fpscr & 0x1f;
    const u32 fpsr = nzcv | idc | fiq;

    // FPSCR bits [26:15] are mapped to FPCR[26:15].
    // FPSCR bits [12:8] are mapped to FPCR[12:8].
    const u32 round = fpscr & 0x7ff8000;
    const u32 trap = fpscr & 0x1f00;
    const u32 fpcr = round | trap;

    return {fpsr, fpcr};
}

static u32 FpsrFpcrToFpscr(u64 fpsr, u64 fpcr) {
    auto [s, c] = FpscrToFpsrFpcr(static_cast<u32>(fpsr | fpcr));
    return s | c;
}

bool ArmDynarmic32::IsInThumbMode() const {
    return (m_jit->Cpsr() & 0x20) != 0;
}

void ArmDynarmic32::Initialize()
{
}

ProcessorArchitecture ArmDynarmic32::GetArchitecture() const
{
    return ProcessorArchitecture::AArch32;
}

CpuHaltReason ArmDynarmic32::RunThread(IKernelThread * thread)
{
    ScopedJitExecution sj(thread->GetOwnerProcess());

    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Run());
}

CpuHaltReason ArmDynarmic32::StepThread(IKernelThread * thread)
{
    ScopedJitExecution sj(thread->GetOwnerProcess());

    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Step());
}

void ArmDynarmic32::LockThread(IKernelThread * thread)
{
}

void ArmDynarmic32::UnlockThread(IKernelThread * thread)
{
}

u32 ArmDynarmic32::GetSvcNumber() const
{
    return m_svc_swi;
}

void ArmDynarmic32::GetSvcArguments(uint64_t (&args)[8]) const
{
    Dynarmic::A32::Jit & j = *m_jit;
    auto & gpr = j.Regs();

    for (size_t i = 0, n = sizeof(args) / sizeof(args[0]); i < n; i++)
    {
        args[i] = gpr[i];
    }
}

void ArmDynarmic32::SetSvcArguments(const uint64_t (&args)[8])
{
    Dynarmic::A32::Jit & j = *m_jit;
    auto & gpr = j.Regs();

    for (size_t i = 0, n = sizeof(args) / sizeof(args[0]); i < n; i++)
    {
        gpr[i] = static_cast<u32>(args[i]);
    }
}

const CpuDebugWatchpoint * ArmDynarmic32::HaltedWatchpoint() const
{
    return m_halted_watchpoint;
}

void ArmDynarmic32::RewindBreakpointInstruction()
{
    this->SetContext(m_breakpoint_context);
}

void ArmDynarmic32::SetWatchpointArray(const CpuDebugWatchpoint * watchpoints, uint32_t count)
{
    for (uint32_t i = 0, n = count < (uint32_t)m_watchpoints.size() ? count : (uint32_t)m_watchpoints.size(); i < n; i++)
    {
        m_watchpoints[i] = watchpoints[i];
    }
}

ArmDynarmic32::ArmDynarmic32(ICoreSystem & system, bool uses_wall_clock, IKernelProcess & process, Dynarmic::ExclusiveMonitor & monitor, uint32_t core_index) :
    m_system{system}, 
    m_monitor{monitor},
    m_cb(std::make_unique<DynarmicCallbacks32>(*this, process)),
    m_cp15(std::make_shared<Core::DynarmicCP15>(*this)), 
    m_core_index{core_index},
    m_uses_wall_clock(uses_wall_clock)
{
    m_jit = MakeJit(process);
    ScopedJitExecution::RegisterHandler();
}

ArmDynarmic32::~ArmDynarmic32() = default;

void ArmDynarmic32::SetTpidrroEl0(u64 value)
{
    m_cp15->uro = static_cast<u32>(value);
}

void ArmDynarmic32::GetContext(CpuThreadContext & ctx) const
{
    Dynarmic::A32::Jit & j = *m_jit;
    auto & gpr = j.Regs();
    auto & fpr = j.ExtRegs();

    for (size_t i = 0; i < 16; i++)
    {
        ctx.r[i] = gpr[i];
    }

    ctx.fp = gpr[11];
    ctx.sp = gpr[13];
    ctx.lr = gpr[14];
    ctx.pc = gpr[15];
    ctx.pstate = j.Cpsr();

    static_assert(sizeof(fpr) <= sizeof(ctx.v));
    std::memcpy(ctx.v, &fpr, sizeof(fpr));

    auto [fpsr, fpcr] = FpscrToFpsrFpcr(j.Fpscr());
    ctx.fpcr = fpcr;
    ctx.fpsr = fpsr;
    ctx.tpidr = m_cp15->uprw;
}

void ArmDynarmic32::SetContext(const CpuThreadContext & ctx)
{
    Dynarmic::A32::Jit & j = *m_jit;
    auto & gpr = j.Regs();
    auto & fpr = j.ExtRegs();

    for (size_t i = 0; i < 16; i++)
    {
        gpr[i] = static_cast<u32>(ctx.r[i]);
    }

    j.SetCpsr(ctx.pstate);

    static_assert(sizeof(fpr) <= sizeof(ctx.v));
    std::memcpy(&fpr, ctx.v, sizeof(fpr));

    j.SetFpscr(FpsrFpcrToFpscr(ctx.fpsr, ctx.fpcr));
    m_cp15->uprw = static_cast<u32>(ctx.tpidr);
}

void ArmDynarmic32::SignalInterrupt(IKernelThread * thread)
{
    m_jit->HaltExecution(TranslateDynarmicHaltReason(CpuHaltReason::BreakLoop));
}

void ArmDynarmic32::ClearInstructionCache()
{
    m_jit->ClearCache();
}

void ArmDynarmic32::InvalidateCacheRange(u64 addr, std::size_t size)
{
    m_jit->InvalidateCacheRange(static_cast<u32>(addr), size);
}

void ArmDynarmic32::Release()
{
    delete this;
}

