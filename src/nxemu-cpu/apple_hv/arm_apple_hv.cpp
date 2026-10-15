// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Architecture based in part on Ryubing/Ryujinx AppleHv (MIT):
// Copyright (c) Ryujinx Team and Contributors.

#include "apple_hv/arm_apple_hv.h"

#include "apple_hv/optimization_flags.h"
#include <cstdlib>
#include <cstring>
#include <mach/mach_time.h>
#include <thread>

#include <Hypervisor/Hypervisor.h>

#include "cpu_settings.h"
#include "yuzu_common/wall_clock.h"

#include "yuzu_common/alignment.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/host_memory.h"
#include "apple_hv/apple_hv_address_space.h"

namespace Core {
namespace {

constexpr u32 EcHvcAarch64 = 0b010110;
constexpr u32 EcSvcAarch64 = 0b010101;
constexpr u32 EcTrappedMsrMrsSystem = 0b011000;
constexpr u32 EcInstructionAbortLowerEl = 0b100000;
constexpr u32 EcDataAbortLowerEl = 0b100100;
constexpr u32 EcSoftwareStepLowerEl = 0b110010;
constexpr u32 EcBrkAarch64 = 0b111100;

constexpr u32 PstateSingleStep = 1U << 21;
constexpr u64 GuestPageSize = 0x1000;

// One reusable vCPU per host execution thread. Guest execution may migrate, but a borrowed
// handle never leaves RunInternal and is returned before entering the HLE scheduler.
// The VM lease outlives address spaces if this thread is still alive during process teardown.
struct ThreadVcpu {
    std::shared_ptr<void> vm;
    hv_vcpu_t handle{};
    hv_vcpu_exit_t* exit{};
    u64 address_space{};
    u64 generation{};
    u64 code_generation{};
    bool valid{};

    ~ThreadVcpu() {
        Reset();
    }

    void Reset() {
        if (valid) {
            const auto result = hv_vcpu_destroy(handle);
            if (result != HV_SUCCESS) {
                LOG_ERROR(Core_ARM, "AppleHv pool: owner-thread destroy failed {:#x}", static_cast<u32>(result));
            }
            valid = false;
        }
        vm.reset();
        address_space = 0;
        generation = 0;
        code_generation = 0;
    }
};

thread_local ThreadVcpu thread_vcpu;

hv_reg_t Gpr(u32 index) {
    return static_cast<hv_reg_t>(static_cast<u32>(HV_REG_X0) + index);
}

hv_simd_fp_reg_t Simd(u32 index) {
    return static_cast<hv_simd_fp_reg_t>(static_cast<u32>(HV_SIMD_FP_REG_Q0) + index);
}

[[nodiscard]] bool ContinueOnVTimerExit() {
    static const bool enabled = [] {
        const char* const value = std::getenv("NXEMU_APPLE_HV_VTIMER_CONTINUE");
        return value == nullptr || std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

} // namespace

ArmAppleHv::ArmAppleHv(ICoreSystem& system, IKernelProcess& process,
                       std::shared_ptr<AppleHv::AddressSpace> address_space, std::size_t core_index)
    : m_system{system}, m_process{process}, m_address_space{std::move(address_space)},
      m_core_index{core_index} {}

ArmAppleHv::~ArmAppleHv() = default;

bool ArmAppleHv::HvSucceeded(u32 result, const char* operation) const {
    if (result == static_cast<u32>(HV_SUCCESS)) {
        return true;
    }
    LOG_ERROR(Core_ARM, "AppleHv core {}: {} failed with status {:#x}", m_core_index, operation,
              result);
    return false;
}

u64 ArmAppleHv::MaintenanceAddress() {
    const u64 generation = m_address_space->CodeGeneration();
    m_code_flush_pending |= generation != m_vcpu_code_generation;
    const bool invalidate_code = m_code_flush_pending;
    m_vcpu_code_generation = generation;
    return invalidate_code ? m_address_space->KernelTlbiEretAddress() : m_address_space->KernelTlbOnlyEretAddress();
}

bool ArmAppleHv::PrepareResume() {
    const u64 generation = m_address_space->Generation();
    if (generation == m_vcpu_generation) {
        return true;
    }
    u64 cpsr{}, pc{};
    if (!HvSucceeded(hv_vcpu_get_reg(m_vcpu, HV_REG_CPSR, &cpsr), "read CPSR for TLBI") ||
        !HvSucceeded(hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &pc), "read PC for TLBI")) {
        return false;
    }
    if ((cpsr & 0xf) == 0) {
        if (!HvSucceeded(hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_ELR_EL1, pc), "TLBI ELR") ||
            !HvSucceeded(hv_vcpu_set_sys_reg(m_vcpu, HV_SYS_REG_SPSR_EL1, cpsr), "TLBI SPSR") ||
            !HvSucceeded(hv_vcpu_set_reg(m_vcpu, HV_REG_CPSR, 0x3c5), "TLBI EL1h")) {
            return false;
        }
    } else if (pc < m_address_space->KernelTlbiEretAddress()) {
        // The EL0 exception is still waiting to execute its HVC. Do not skip it.
        return true;
    }
    if (!HvSucceeded(hv_vcpu_set_reg(m_vcpu, HV_REG_PC,
                                   MaintenanceAddress()), "TLBI stub")) {
        return false;
    }
    m_vcpu_generation = generation;
    return true;
}

bool ArmAppleHv::CreateVcpu() {
    std::scoped_lock lk{m_vcpu_lifetime_mutex};
    if (m_vcpu != InvalidVcpu) {
        return true;
    }

    if (!thread_vcpu.valid) {
        thread_vcpu.vm = AppleHv::AddressSpace::RetainVm();
        if (!HvSucceeded(hv_vcpu_create(&thread_vcpu.handle, &thread_vcpu.exit, nullptr), "pool hv_vcpu_create")) {
            thread_vcpu.vm.reset();
            return false;
        }
        thread_vcpu.valid = true;
        LOG_INFO(Core_ARM, "AppleHv pool: created host-thread vCPU {}", thread_vcpu.handle);
    }
    const hv_vcpu_t vcpu = thread_vcpu.handle;
    m_vcpu = vcpu;
    m_exit_info = thread_vcpu.exit;
    const auto fail = [&] {
        thread_vcpu.Reset();
        m_vcpu = InvalidVcpu;
        m_exit_info = nullptr;
    };

    if (!HvSucceeded(static_cast<u32>(
                         hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, 3ULL << 20)),
                     "hv_vcpu_set_sys_reg(CPACR_EL1)")) {
        fail();
        return false;
    }
    const bool changed_space = thread_vcpu.address_space != m_address_space->Identity();
    if (changed_space && !m_address_space->InitializeVcpu(m_vcpu)) {
        LOG_ERROR(Core_ARM, "AppleHv core {}: failed to initialize guest MMU", m_core_index);
        fail();
        return false;
    }

    // Enabling direct CNTVCT also exposes the host CNTFRQ to EL0. Keep both timer
    // access bits clear so all guest counter reads use the same 19.2 MHz clock.
    if (!HvSucceeded(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CNTKCTL_EL1, 0), "trap guest counters")) {
        fail();
        return false;
    }
    if (!LoadContextToVcpu()) {
        fail();
        return false;
    }

    // A bounded slice also covers interrupts racing with entry into hv_vcpu_run. VTimer exits can
    // normally be re-armed in-place instead of forcing a full HLE scheduler round-trip.
    if (!ArmVTimer() ||
        !HvSucceeded(hv_vcpu_set_trap_debug_exceptions(vcpu, false), "guest debug exceptions")) {
        fail();
        return false;
    }

    if (!HvSucceeded(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, 0), "reset debug state")) {
        fail();
        return false;
    }
    for (u32 i = 0; i < 4; ++i) {
        if (!HvSucceeded(hv_vcpu_set_sys_reg(vcpu,
                static_cast<hv_sys_reg_t>(HV_SYS_REG_DBGWCR0_EL1 + i * 8), 0), "reset watchpoint")) {
            fail();
            return false;
        }
    }

    // Program the four hardware watchpoints with byte-select comparators instead of
    // removing access to an entire 4 KiB page (which also faults unrelated accesses).
    if (m_watchpoints) {
        for (u32 i = 0; i < m_watchpoint_count; ++i) {
            const auto& watch = m_watchpoints[i];
            if (watch.type == CpuDebugWatchpointType::None) {
                continue;
            }
            const u64 start = watch.startAddress;
            const u64 size = watch.endAddress - start;
            if (size == 0 || (start & 7) + size > 8) {
                LOG_ERROR(Core_ARM, "AppleHv: unsupported watchpoint range {:#x}+{}", start, size);
                fail();
                return false;
            }
            const u64 bas = ((1ULL << size) - 1) << (start & 7);
            const u64 control = 1 | (2ULL << 1) | (static_cast<u64>(watch.type) << 3) | (bas << 5);
            if (!HvSucceeded(hv_vcpu_set_sys_reg(vcpu,
                    static_cast<hv_sys_reg_t>(HV_SYS_REG_DBGWVR0_EL1 + i * 8), start & ~7ULL), "watch address") ||
                !HvSucceeded(hv_vcpu_set_sys_reg(vcpu,
                    static_cast<hv_sys_reg_t>(HV_SYS_REG_DBGWCR0_EL1 + i * 8), control), "watch control") ||
                !HvSucceeded(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, 1ULL << 15), "monitor debug")) {
                fail();
                return false;
            }
        }
    }

    m_address_space->RegisterVcpu(m_vcpu, m_running);
    m_vcpu_generation = changed_space ? 0 : thread_vcpu.generation;
    m_vcpu_code_generation = changed_space ? 0 : thread_vcpu.code_generation;
    m_code_flush_pending = false;
    thread_vcpu.address_space = m_address_space->Identity();
    m_published_vcpu.store(m_vcpu, std::memory_order_release);
    return true;
}

bool ArmAppleHv::ArmVTimer() {
    const hv_vcpu_t vcpu = static_cast<hv_vcpu_t>(m_vcpu);
    static const u64 timer_delta = [] {
        mach_timebase_info_data_t timebase{};
        if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) {
            return u64{0};
        }
        constexpr u64 SliceIntervalNs = 10'000'000ULL;
        return (SliceIntervalNs * timebase.denom + timebase.numer - 1) / timebase.numer;
    }();
    if (timer_delta == 0) {
        LOG_ERROR(Core_ARM, "AppleHv core {}: unable to derive VTimer timebase", m_core_index);
        return false;
    }

    const u64 deadline = mach_absolute_time() + timer_delta;
    return HvSucceeded(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CNTV_CVAL_EL0, deadline),
                       "timer deadline") &&
           HvSucceeded(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_CNTV_CTL_EL0, 1), "timer enable") &&
           HvSucceeded(hv_vcpu_set_vtimer_mask(vcpu, false), "timer unmask");
}

void ArmAppleHv::DestroyVcpu() {
    std::scoped_lock lk{m_vcpu_lifetime_mutex};
    if (m_vcpu == InvalidVcpu) {
        return;
    }

    m_published_vcpu.store(InvalidVcpu, std::memory_order_release);
    m_address_space->UnregisterVcpu(m_vcpu);
    u64 pc{}, cpsr{};
    const bool pending_maintenance = hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &pc) != HV_SUCCESS ||
        hv_vcpu_get_reg(m_vcpu, HV_REG_CPSR, &cpsr) != HV_SUCCESS ||
        ((cpsr & 0xf) != 0 && pc >= m_address_space->KernelTlbiEretAddress());
    thread_vcpu.generation = pending_maintenance ? 0 : m_vcpu_generation;
    thread_vcpu.code_generation = pending_maintenance ? 0 : m_vcpu_code_generation;
    m_vcpu = InvalidVcpu;
    m_exit_info = nullptr;
}

bool ArmAppleHv::LoadContextToVcpu() {
    const hv_vcpu_t vcpu = static_cast<hv_vcpu_t>(m_vcpu);
    for (u32 i = 0; i < 29; ++i) {
        if (!HvSucceeded(static_cast<u32>(hv_vcpu_set_reg(vcpu, Gpr(i), m_context.r[i])),
                         "hv_vcpu_set_reg(X)")) {
            return false;
        }
    }
    if (!HvSucceeded(static_cast<u32>(hv_vcpu_set_reg(vcpu, HV_REG_X29, m_context.fp)),
                     "hv_vcpu_set_reg(FP)" ) ||
        !HvSucceeded(static_cast<u32>(hv_vcpu_set_reg(vcpu, HV_REG_X30, m_context.lr)),
                     "hv_vcpu_set_reg(LR)") ||
        !HvSucceeded(static_cast<u32>(hv_vcpu_set_reg(vcpu, HV_REG_PC, m_context.pc)),
                     "hv_vcpu_set_reg(PC)") ||
        !HvSucceeded(static_cast<u32>(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, m_context.pstate)),
                     "hv_vcpu_set_reg(CPSR)") ||
        !HvSucceeded(static_cast<u32>(hv_vcpu_set_reg(vcpu, HV_REG_FPCR, m_context.fpcr)),
                     "hv_vcpu_set_reg(FPCR)") ||
        !HvSucceeded(static_cast<u32>(hv_vcpu_set_reg(vcpu, HV_REG_FPSR, m_context.fpsr)),
                     "hv_vcpu_set_reg(FPSR)") ||
        !HvSucceeded(static_cast<u32>(
                         hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL0, m_context.sp)),
                     "hv_vcpu_set_sys_reg(SP_EL0)") ||
        !HvSucceeded(static_cast<u32>(
                         hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, m_context.tpidr)),
                     "hv_vcpu_set_sys_reg(TPIDR_EL0)") ||
        !HvSucceeded(static_cast<u32>(
                         hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TPIDRRO_EL0, m_tpidrro_el0)),
                     "hv_vcpu_set_sys_reg(TPIDRRO_EL0)")) {
        return false;
    }

    for (u32 i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t value{};
        static_assert(sizeof(value) == sizeof(u128));
        std::memcpy(&value, m_context.v[i], sizeof(value));
        if (!HvSucceeded(
                static_cast<u32>(hv_vcpu_set_simd_fp_reg(vcpu, Simd(i), value)),
                "hv_vcpu_set_simd_fp_reg(Q)")) {
            return false;
        }
    }
    return true;
}

bool ArmAppleHv::SaveCurrentEl0Context() {
    const hv_vcpu_t vcpu = static_cast<hv_vcpu_t>(m_vcpu);
    u64 value{};

    for (u32 i = 0; i < 29; ++i) {
        if (!HvSucceeded(static_cast<u32>(hv_vcpu_get_reg(vcpu, Gpr(i), &m_context.r[i])),
                         "hv_vcpu_get_reg(X)")) {
            return false;
        }
    }
    if (!HvSucceeded(static_cast<u32>(hv_vcpu_get_reg(vcpu, HV_REG_X29, &m_context.fp)),
                     "hv_vcpu_get_reg(FP)") ||
        !HvSucceeded(static_cast<u32>(hv_vcpu_get_reg(vcpu, HV_REG_X30, &m_context.lr)),
                     "hv_vcpu_get_reg(LR)") ||
        !HvSucceeded(static_cast<u32>(hv_vcpu_get_reg(vcpu, HV_REG_PC, &m_context.pc)),
                     "hv_vcpu_get_reg(PC)") ||
        !HvSucceeded(static_cast<u32>(hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &value)),
                     "hv_vcpu_get_reg(CPSR)")) {
        return false;
    }
    m_context.pstate = static_cast<u32>(value);

    if (!HvSucceeded(static_cast<u32>(hv_vcpu_get_reg(vcpu, HV_REG_FPCR, &value)),
                     "hv_vcpu_get_reg(FPCR)")) {
        return false;
    }
    m_context.fpcr = static_cast<u32>(value);
    if (!HvSucceeded(static_cast<u32>(hv_vcpu_get_reg(vcpu, HV_REG_FPSR, &value)),
                     "hv_vcpu_get_reg(FPSR)")) {
        return false;
    }
    m_context.fpsr = static_cast<u32>(value);

    if (!HvSucceeded(static_cast<u32>(
                         hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL0, &m_context.sp)),
                     "hv_vcpu_get_sys_reg(SP_EL0)") ||
        !HvSucceeded(static_cast<u32>(
                         hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TPIDR_EL0, &m_context.tpidr)),
                     "hv_vcpu_get_sys_reg(TPIDR_EL0)")) {
        return false;
    }

    for (u32 i = 0; i < 32; ++i) {
        hv_simd_fp_uchar16_t simd{};
        if (!HvSucceeded(static_cast<u32>(hv_vcpu_get_simd_fp_reg(vcpu, Simd(i), &simd)),
                         "hv_vcpu_get_simd_fp_reg(Q)")) {
            return false;
        }
        std::memcpy(m_context.v[i], &simd, sizeof(simd));
    }
    return true;
}

bool ArmAppleHv::SaveExceptionEl0Context(u64 resume_pc) {
    // General/SIMD registers and SP_EL0 still contain the guest EL0 state while the tiny EL1
    // vector is stopped on HVC. PC/PSTATE must instead come from ELR_EL1/SPSR_EL1.
    if (!SaveCurrentEl0Context()) {
        return false;
    }

    const hv_vcpu_t vcpu = static_cast<hv_vcpu_t>(m_vcpu);
    u64 spsr{};
    if (!HvSucceeded(static_cast<u32>(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, &spsr)),
                     "hv_vcpu_get_sys_reg(SPSR_EL1)")) {
        return false;
    }
    m_context.pc = resume_pc;
    m_context.pstate = static_cast<u32>(spsr);
    return true;
}

bool ArmAppleHv::HandleTrappedSystemRegister(u64 esr) {
    const bool read = (esr & 1ULL) != 0;
    const u32 rt = static_cast<u32>((esr >> 5) & 0x1f);
    if (!read) {
        LOG_ERROR(Core_ARM, "AppleHv: unhandled trapped system-register write, ESR={:#x}", esr);
        return false;
    }

    u64 value{};
    switch ((esr >> 1) & 0x1ffe0fULL) {
    case 0b11'000'011'1110'00000'0000: // CNTFRQ_EL0
        value = Common::WallClock::CNTFRQ;
        break;
    case 0b11'010'011'1110'00000'0000: // CNTVCT_EL0
        value = m_system.Timing().GetClockTicks();
        break;
    case 0b11'001'011'1110'00000'0000: // CNTPCT_EL0
        value = m_system.Timing().GetClockTicks();
        break;
    default:
        LOG_ERROR(Core_ARM, "AppleHv: unhandled trapped system-register read, ESR={:#x}", esr);
        return false;
    }

    // While stopped in the EL1 HVC vector, X0-X30 still contain the interrupted EL0 register
    // state. Write the trapped MRS destination directly and return through ERET.
    if (rt < 31) {
        const hv_vcpu_t vcpu = static_cast<hv_vcpu_t>(m_vcpu);
        if (!HvSucceeded(static_cast<u32>(hv_vcpu_set_reg(vcpu, Gpr(rt), value)),
                         "hv_vcpu_set_reg(trapped MRS)")) {
            return false;
        }
    }
    return true;
}

std::optional<CpuHaltReason> ArmAppleHv::HandleSynchronousException() {
    const hv_vcpu_t vcpu = static_cast<hv_vcpu_t>(m_vcpu);
    u64 esr{};
    u64 elr{};
    if (!HvSucceeded(static_cast<u32>(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ESR_EL1, &esr)),
                     "hv_vcpu_get_sys_reg(ESR_EL1)") ||
        !HvSucceeded(static_cast<u32>(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &elr)),
                     "hv_vcpu_get_sys_reg(ELR_EL1)")) {
        return CpuHaltReason::PrefetchAbort;
    }

    const u32 ec = static_cast<u32>(esr >> 26);
    switch (ec) {
    case EcSvcAarch64:
        if (!SaveExceptionEl0Context(elr)) {
            return CpuHaltReason::PrefetchAbort;
        }
        m_svc_number = static_cast<u32>(esr & 0xffff);
        return CpuHaltReason::SupervisorCall;

    case EcTrappedMsrMrsSystem:
        if (!HandleTrappedSystemRegister(esr) ||
            !HvSucceeded(static_cast<u32>(
                             hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, elr + 4)),
                         "hv_vcpu_set_sys_reg(ELR_EL1 after MRS)") ||
            !HvSucceeded(static_cast<u32>(hv_vcpu_set_reg(
                             vcpu, HV_REG_PC, m_address_space->KernelEretAddress())),
                         "hv_vcpu_set_reg(ERET stub)")) {
            return CpuHaltReason::PrefetchAbort;
        }
        return std::nullopt;

    case EcDataAbortLowerEl: {
        u64 far{};
        const bool far_valid = (esr & (1ULL << 10)) == 0;
        if (!far_valid ||
            !HvSucceeded(static_cast<u32>(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1, &far)),
                         "hv_vcpu_get_sys_reg(FAR_EL1)")) {
            LOG_ERROR(Core_ARM, "AppleHv: data abort without a valid FAR, ESR={:#x}", esr);
            return CpuHaltReason::PrefetchAbort;
        }

        const bool write = (esr & (1ULL << 6)) != 0;
        const u64 access_size = 1ULL << ((esr >> 22) & 3ULL);
        const auto access_type = write ? CpuDebugWatchpointType::Write
                                       : CpuDebugWatchpointType::Read;
        if (const auto* watchpoint = MatchingWatchpoint(far, access_size, access_type)) {
            m_halted_watchpoint = watchpoint;
            SaveExceptionEl0Context(elr);
            return CpuHaltReason::DataAbort;
        }

        // Rasterizer tracking is page-granular. Resolve the full page and preserve whether the
        // faulting guest access was a read or a write so NCE can keep read-only pages materialized.
        const u64 page = Common::AlignDown(far, GuestPageSize);
        const u32 fault_status = static_cast<u32>(esr & 0x3f);
        const bool mapping_fault = (fault_status >= 0x4 && fault_status <= 0xf);
        if (mapping_fault && (((AppleHv::OptimizationFlags() & AppleHv::ResolveFaultImmediately) == 0 && m_address_space->Generation() != m_vcpu_generation) ||
            m_address_space->AllowsAccess(far, write ? Common::MemoryPermission::Write : Common::MemoryPermission::Read) ||
            m_process.GetMemory().InvalidateNCEAccess(page, GuestPageSize, write) ||
            // Software cache eviction can precede reopening the hardware page.
            // Recheck after invalidation repaired that permission-only race.
            m_address_space->AllowsAccess(far, write ? Common::MemoryPermission::Write : Common::MemoryPermission::Read))) {
            // Memory tracking may have changed stage-1 permissions. Return through a small EL1
            // TLBI+ERET stub so stale translations cannot survive the fault handling.
            if (!HvSucceeded(static_cast<u32>(hv_vcpu_set_reg(
                                 vcpu, HV_REG_PC, m_address_space->KernelEretAddress())),
                             "hv_vcpu_set_reg(TLBI+ERET stub)")) {
                return CpuHaltReason::PrefetchAbort;
            }
            // Snapshot generations only under LockExecution in PrepareResume. A writer may
            // otherwise publish a new mapping between selecting the stub and reading its epoch.
            m_vcpu_generation = 0;
            return std::nullopt;
        }

        // JIT compatibility settings must not skip a native instruction: its destination
        // registers and address writeback would otherwise silently retain stale values.
        m_address_space->LogFault(far);
        LOG_ERROR(Core_ARM, "AppleHv: unhandled guest data abort at {:#x}, ESR={:#x}", far, esr);
        SaveExceptionEl0Context(elr);
        return CpuHaltReason::PrefetchAbort;
    }

    case EcInstructionAbortLowerEl: {
        const u32 fault_status = static_cast<u32>(esr & 0x3f);
        // Reactive GPU tracking can also protect a page containing executable code.
        if (fault_status >= 0x4 && fault_status <= 0xf &&
            (m_address_space->Generation() != m_vcpu_generation ||
             m_process.GetMemory().InvalidateNCEAccess(Common::AlignDown(elr, GuestPageSize),
                                                      GuestPageSize, false) ||
             m_address_space->AllowsAccess(elr, Common::MemoryPermission::Execute))) {
            if (!HvSucceeded(hv_vcpu_set_reg(vcpu, HV_REG_PC,
                    m_address_space->KernelEretAddress()), "tracked instruction fetch return")) {
                return CpuHaltReason::PrefetchAbort;
            }
            m_vcpu_generation = 0;
            return std::nullopt;
        }
        SaveExceptionEl0Context(elr);
        return CpuHaltReason::PrefetchAbort;

    }

    case EcSoftwareStepLowerEl:
        if (!SaveExceptionEl0Context(elr)) {
            return CpuHaltReason::PrefetchAbort;
        }
        m_context.pstate &= ~PstateSingleStep;
        return CpuHaltReason::StepThread;

    case 0b110100: { // Watchpoint from EL0.
        u64 far{};
        if (!HvSucceeded(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_FAR_EL1, &far), "watchpoint FAR") ||
            !SaveExceptionEl0Context(elr)) {
            return CpuHaltReason::PrefetchAbort;
        }
        m_halted_watchpoint = MatchingWatchpoint(far, 1, (esr & (1ULL << 6))
            ? CpuDebugWatchpointType::Write : CpuDebugWatchpointType::Read);
        return CpuHaltReason::DataAbort;
    }

    case EcBrkAarch64:
        if (!SaveExceptionEl0Context(elr)) {
            return CpuHaltReason::PrefetchAbort;
        }
        return CpuHaltReason::InstructionBreakpoint;

    default:
        LOG_ERROR(Core_ARM, "AppleHv: unhandled guest exception class {:#x}, ESR={:#x}, ELR={:#x}",
                  ec, esr, elr);
        SaveExceptionEl0Context(elr);
        return CpuHaltReason::PrefetchAbort;
    }
}

CpuHaltReason ArmAppleHv::RunInternal(bool single_step) {
    m_halted_watchpoint = nullptr;


    if (!CreateVcpu()) {
        return CpuHaltReason::PrefetchAbort;
    }

    struct VcpuGuard {
        ArmAppleHv* self;
        ~VcpuGuard() {
            self->DestroyVcpu();
        }
    } guard{this};

    if (single_step) {
        const hv_vcpu_t vcpu = static_cast<hv_vcpu_t>(m_vcpu);
        if (!HvSucceeded(static_cast<u32>(
                             hv_vcpu_set_reg(vcpu, HV_REG_CPSR,
                                             static_cast<u64>(m_context.pstate | PstateSingleStep))),
                         "hv_vcpu_set_reg(CPSR single-step)") ||
            !HvSucceeded(static_cast<u32>(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, (1ULL << 15) | 1)),
                         "hv_vcpu_set_sys_reg(MDSCR_EL1)")) {
            return CpuHaltReason::PrefetchAbort;
        }
    }

    while (true) {
        auto execution_lock = m_address_space->LockExecution(m_running);
        struct RunningGuard {
            std::atomic_bool& running;
            ~RunningGuard() { running.store(false, std::memory_order_release); }
        } running_guard{m_running};
        if (!PrepareResume()) {
            return CpuHaltReason::PrefetchAbort;
        }
        u64 cpsr{};
        if (!HvSucceeded(hv_vcpu_get_reg(m_vcpu, HV_REG_CPSR, &cpsr), "read execution mode")) {
            return CpuHaltReason::PrefetchAbort;
        }
        if (m_interrupt_requested.load()) {
            u64 pc{};
            if (!HvSucceeded(hv_vcpu_get_reg(m_vcpu, HV_REG_PC, &pc), "interrupt PC")) {
                return CpuHaltReason::PrefetchAbort;
            }
            if ((cpsr & 0xf) == 0) {
                m_interrupt_requested.store(false);
                return SaveCurrentEl0Context() ? CpuHaltReason::BreakLoop : CpuHaltReason::PrefetchAbort;
            }
            if (pc >= m_address_space->KernelTlbiEretAddress() &&
                pc <= m_address_space->KernelTlbOnlyEretAddress() + 20) {
                // The exception has been handled; only maintenance/ERET remains. Save the
                // architectural return state, then recreate and flush on the next slice.
                u64 elr{};
                m_interrupt_requested.store(false);
                return HvSucceeded(hv_vcpu_get_sys_reg(m_vcpu, HV_SYS_REG_ELR_EL1, &elr), "interrupt ELR") &&
                    SaveExceptionEl0Context(elr) ? CpuHaltReason::BreakLoop : CpuHaltReason::PrefetchAbort;
            }
            // A vector still has a pending exception: execute its HVC before yielding.
        }

        const hv_vcpu_t vcpu = static_cast<hv_vcpu_t>(m_vcpu);
        const hv_return_t run_result = hv_vcpu_run(vcpu);
        if (!HvSucceeded(static_cast<u32>(run_result), "hv_vcpu_run")) {
            SaveCurrentEl0Context();
            return CpuHaltReason::PrefetchAbort;
        }

        if (m_code_flush_pending) {
            u64 resume_pc{}, resume_pstate{};
            if (!HvSucceeded(hv_vcpu_get_reg(vcpu, HV_REG_PC, &resume_pc), "maintenance completion PC") ||
                !HvSucceeded(hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &resume_pstate), "maintenance completion PSTATE")) {
                return CpuHaltReason::PrefetchAbort;
            }
            if ((resume_pstate & 0xf) == 0 || resume_pc < m_address_space->KernelTlbiEretAddress()) {
                m_code_flush_pending = false;
            }
        }
        // A vCPU handling a fault/SVC cannot execute guest code. Canceling it here
        // would unnecessarily cancel its next entry into the hypervisor instead.
        m_running.store(false, std::memory_order_release);
        execution_lock.unlock();
        if (!m_logged_run) {
            LOG_INFO(Core_ARM, "AppleHv core {}: native AArch64 execution started", m_core_index);
            m_logged_run = true;
        }
        const auto* exit_info = static_cast<const hv_vcpu_exit_t*>(m_exit_info);
        LOG_TRACE(Core_ARM, "AppleHv core {}: exit={} syndrome={:#x}", m_core_index,
                  static_cast<u32>(exit_info->reason), exit_info->exception.syndrome);
        switch (exit_info->reason) {
        case HV_EXIT_REASON_EXCEPTION: {
            const u32 hv_ec = static_cast<u32>(exit_info->exception.syndrome >> 26);
            if (hv_ec != EcHvcAarch64) {
                LOG_ERROR(Core_ARM, "AppleHv: unexpected hypervisor exception EC={:#x}, ESR={:#x}",
                          hv_ec, exit_info->exception.syndrome);
                SaveCurrentEl0Context();
                return CpuHaltReason::PrefetchAbort;
            }

            const auto reason = HandleSynchronousException();
            if (reason) {
                return *reason;
            }

            // PrepareResume samples both generations under the execution lock before entry.
            break;
        }

        case HV_EXIT_REASON_CANCELED:
            // The cancellation can land in an EL1 vector or maintenance stub.
            // Preserve the live vCPU and let the next iteration inspect its execution mode.
            break;

        case HV_EXIT_REASON_VTIMER_ACTIVATED: {
            // Hypervisor.framework masks the VTimer on exit. A watchdog expiry by itself is only a
            // bounded execution slice, not a guest interrupt, so keep the live vCPU and re-arm it.
            // NXEMU_APPLE_HV_VTIMER_CONTINUE=0 restores the previous scheduler-exit behaviour for
            // controlled A/B testing. A real pending interrupt still takes the scheduler path.
            const bool interrupt_pending =
                m_interrupt_requested.load(std::memory_order_acquire);

            if (interrupt_pending || !ContinueOnVTimerExit()) {
                if (!HvSucceeded(hv_vcpu_set_vtimer_mask(vcpu, true), "timer mask")) {
                    SaveCurrentEl0Context();
                    return CpuHaltReason::PrefetchAbort;
                }
                m_interrupt_requested.store(true, std::memory_order_release);
                break;
            }

            if (!ArmVTimer()) {
                SaveCurrentEl0Context();
                return CpuHaltReason::PrefetchAbort;
            }

            // Periodically yield so a very fast native guest cannot starve the
            // MoltenVK submission/presentation workers after removing the scheduler round-trip.
            if (++m_vtimer_continue_count % 12 == 0) {
                std::this_thread::yield();
            }
            break;
        }

        default:
            LOG_ERROR(Core_ARM, "AppleHv: unhandled vCPU exit reason {}",
                      static_cast<u32>(exit_info->reason));
            SaveCurrentEl0Context();
            return CpuHaltReason::PrefetchAbort;
        }
    }
}

CpuHaltReason ArmAppleHv::RunThread(IKernelThread* thread) {
    (void)thread;
    return RunInternal(false);
}

CpuHaltReason ArmAppleHv::StepThread(IKernelThread* thread) {
    (void)thread;
    return RunInternal(true);
}

void ArmAppleHv::ClearInstructionCache() {
    m_address_space->InvalidateCode();
}

void ArmAppleHv::InvalidateCacheRange(u64 addr, u64 size) {
    (void)addr;
    (void)size;
    m_address_space->InvalidateCode();
}

void ArmAppleHv::GetContext(CpuThreadContext& ctx) const {
    ctx = m_context;
}

void ArmAppleHv::SetContext(const CpuThreadContext& ctx) {
    m_context = ctx;
}

void ArmAppleHv::SetTpidrroEl0(u64 value) {
    m_tpidrro_el0 = value;
}

void ArmAppleHv::GetSvcArguments(u64 (&args)[8]) const {
    for (size_t i = 0; i < std::size(args); ++i) {
        args[i] = m_context.r[i];
    }
}

void ArmAppleHv::SetSvcArguments(const u64 (&args)[8]) {
    for (size_t i = 0; i < std::size(args); ++i) {
        m_context.r[i] = args[i];
    }
}

u32 ArmAppleHv::GetSvcNumber() const {
    return m_svc_number;
}

void ArmAppleHv::SignalInterrupt(IKernelThread* thread) {
    (void)thread;
    m_interrupt_requested.store(true, std::memory_order_release);

    std::scoped_lock lk{m_vcpu_lifetime_mutex};
    const u64 published = m_published_vcpu.load(std::memory_order_acquire);
    if (published != InvalidVcpu) {
        hv_vcpu_t vcpu = static_cast<hv_vcpu_t>(published);
        const hv_return_t result = hv_vcpus_exit(&vcpu, 1);
        if (result != HV_SUCCESS) {
            LOG_WARNING(Core_ARM, "AppleHv core {}: interrupt hv_vcpus_exit failed with {:#x}",
                        m_core_index, static_cast<u32>(result));
        }
    }
}

const CpuDebugWatchpoint* ArmAppleHv::MatchingWatchpoint(
    u64 addr, u64 size, CpuDebugWatchpointType access_type) const {
    if (!m_watchpoints || size == 0) {
        return nullptr;
    }
    const u64 end = addr + size;
    if (end < addr) {
        return nullptr;
    }
    const u8 access = static_cast<u8>(access_type);
    for (u32 i = 0; i < m_watchpoint_count; ++i) {
        const auto& watch = m_watchpoints[i];
        if (end <= watch.startAddress || addr >= watch.endAddress) {
            continue;
        }
        if ((access & static_cast<u8>(watch.type)) == 0) {
            continue;
        }
        return &watch;
    }
    return nullptr;
}

void ArmAppleHv::SetWatchpointArray(const CpuDebugWatchpoint* watchpoints, u32 count) {
    m_watchpoints = watchpoints;
    m_watchpoint_count = count;
}

const CpuDebugWatchpoint* ArmAppleHv::HaltedWatchpoint() const {
    return m_halted_watchpoint;
}

void ArmAppleHv::RewindBreakpointInstruction() {
    // BRK is a synchronous exception and ELR_EL1 already identifies the faulting guest instruction.
    // HandleSynchronousException stores that address directly in m_context.pc.
}

} // namespace Core
