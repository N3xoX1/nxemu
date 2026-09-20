// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <stdint.h>

#include "arm_dynarmic.h"
#include "nce/arm_nce.h"
#include "nce/interpreter_visitor.h"
#include "nce/patcher.h"
#include "yuzu_common/literals.h"
#include "yuzu_common/signal_chain.h"
#include "yuzu_common/yuzu_assert.h"

#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace Core
{

namespace
{

struct sigaction g_orig_bus_action;
struct sigaction g_orig_segv_action;

// Verify assembly offsets.
static_assert(offsetof(NativeExecutionParameters, native_context) == TpidrEl0NativeContext);
static_assert(offsetof(NativeExecutionParameters, lock) == TpidrEl0Lock);
static_assert(offsetof(NativeExecutionParameters, magic) == TpidrEl0TlsMagic);
static_assert(NativeExecutionParameters{}.magic == TlsMagic);

fpsimd_context * GetFloatingPointState(mcontext_t & host_ctx)
{
    _aarch64_ctx * header = reinterpret_cast<_aarch64_ctx *>(&host_ctx.__reserved);
    while (header->magic != FPSIMD_MAGIC)
    {
        header = reinterpret_cast<_aarch64_ctx *>(reinterpret_cast<char *>(header) + header->size);
    }
    return reinterpret_cast<fpsimd_context *>(header);
}

using namespace Common::Literals;
constexpr u32 StackSize = 128_KiB;
constexpr u64 NcePageSize = 0x1000;
constexpr u64 NcePageMask = NcePageSize - 1;

} // namespace

void * ArmNce::RestoreGuestContext(void * raw_context)
{
    mcontext_t & host_ctx = static_cast<ucontext_t *>(raw_context)->uc_mcontext;

    NativeExecutionParameters * tpidr = reinterpret_cast<NativeExecutionParameters *>(host_ctx.regs[9]);
    GuestContext * guest_ctx = static_cast<GuestContext *>(tpidr->native_context);

    fpsimd_context * fpctx = GetFloatingPointState(host_ctx);

    std::memcpy(guest_ctx->host_ctx.host_saved_vregs.data(), &fpctx->vregs[8], sizeof(guest_ctx->host_ctx.host_saved_vregs));
    std::memcpy(guest_ctx->host_ctx.host_saved_regs.data(), &host_ctx.regs[19], sizeof(guest_ctx->host_ctx.host_saved_regs));

    guest_ctx->host_ctx.host_sp = host_ctx.sp;

    host_ctx.sp = guest_ctx->sp;
    host_ctx.pc = guest_ctx->pc;
    host_ctx.pstate = guest_ctx->pstate;
    fpctx->fpcr = guest_ctx->fpcr;
    fpctx->fpsr = guest_ctx->fpsr;
    std::memcpy(host_ctx.regs, guest_ctx->cpu_registers.data(), sizeof(host_ctx.regs));
    std::memcpy(fpctx->vregs, guest_ctx->vector_registers.data(), sizeof(fpctx->vregs));
    return tpidr;
}

void ArmNce::SaveGuestContext(GuestContext * guest_ctx, void * raw_context)
{
    mcontext_t & host_ctx = static_cast<ucontext_t *>(raw_context)->uc_mcontext;
    fpsimd_context * fpctx = GetFloatingPointState(host_ctx);

    std::memcpy(guest_ctx->cpu_registers.data(), host_ctx.regs, sizeof(host_ctx.regs));
    std::memcpy(guest_ctx->vector_registers.data(), fpctx->vregs, sizeof(fpctx->vregs));
    guest_ctx->fpsr = fpctx->fpsr;
    guest_ctx->fpcr = fpctx->fpcr;
    guest_ctx->pstate = static_cast<u32>(host_ctx.pstate);
    guest_ctx->pc = host_ctx.pc;
    guest_ctx->sp = host_ctx.sp;

    host_ctx.sp = guest_ctx->host_ctx.host_sp;

    std::memcpy(&host_ctx.regs[19], guest_ctx->host_ctx.host_saved_regs.data(), sizeof(guest_ctx->host_ctx.host_saved_regs));
    std::memcpy(&fpctx->vregs[8], guest_ctx->host_ctx.host_saved_vregs.data(), sizeof(guest_ctx->host_ctx.host_saved_vregs));

    host_ctx.pc = guest_ctx->host_ctx.host_saved_regs[11];
    host_ctx.regs[0] = guest_ctx->esr_el1.exchange(0);
}

bool ArmNce::HandleFailedGuestFault(GuestContext * guest_ctx, void * raw_info, void * raw_context)
{
    mcontext_t & host_ctx = static_cast<ucontext_t *>(raw_context)->uc_mcontext;
    siginfo_t * info = static_cast<siginfo_t *>(raw_info);

    const bool is_prefetch_abort = host_ctx.pc == reinterpret_cast<u64>(info->si_addr);

    if (!is_prefetch_abort)
    {
        host_ctx.pc += 4;
        return true;
    }

    guest_ctx->esr_el1.fetch_or((uint64_t)TranslateDynarmicHaltReason(CpuHaltReason::PrefetchAbort));

    NativeExecutionParameters & thread_params = guest_ctx->parent->m_running_thread->GetNativeExecutionParameters();
    thread_params.lock = SpinLockLocked;

    SaveGuestContext(guest_ctx, raw_context);
    return false;
}

bool ArmNce::HandleGuestAlignmentFault(GuestContext * guest_ctx, void * raw_info, void * raw_context)
{
    mcontext_t & host_ctx = static_cast<ucontext_t *>(raw_context)->uc_mcontext;
    fpsimd_context * fpctx = GetFloatingPointState(host_ctx);
    std::optional<u64> next_pc = MatchAndExecuteOneInstruction(*guest_ctx->memory, &host_ctx, fpctx);
    if (next_pc)
    {
        host_ctx.pc = *next_pc;
        return true;
    }
    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

bool ArmNce::HandleGuestAccessFault(GuestContext * guest_ctx, void * raw_info, void * raw_context)
{
    siginfo_t * info = static_cast<siginfo_t *>(raw_info);

    const uint64_t addr = (reinterpret_cast<u64>(info->si_addr) & ~NcePageMask);
    if (guest_ctx->memory->InvalidateNCE(addr, NcePageSize))
    {
        // We handled the access successfully and are returning to guest code.
        return true;
    }

    return HandleFailedGuestFault(guest_ctx, raw_info, raw_context);
}

void ArmNce::HandleHostAlignmentFault(int sig, void * raw_info, void * raw_context)
{
    return g_orig_bus_action.sa_sigaction(sig, static_cast<siginfo_t *>(raw_info), raw_context);
}

void ArmNce::HandleHostAccessFault(int sig, void * raw_info, void * raw_context)
{
    return g_orig_segv_action.sa_sigaction(sig, static_cast<siginfo_t *>(raw_info), raw_context);
}

void ArmNce::LockThread(IKernelThread * thread)
{
    NativeExecutionParameters * thread_params = &thread->GetNativeExecutionParameters();
    LockThreadParameters(thread_params);
}

void ArmNce::UnlockThread(IKernelThread * thread)
{
    NativeExecutionParameters * thread_params = &thread->GetNativeExecutionParameters();
    UnlockThreadParameters(thread_params);
}

CpuHaltReason ArmNce::RunThread(IKernelThread * thread)
{
    Dynarmic::HaltReason hr = static_cast<Dynarmic::HaltReason>(m_guest_ctx.esr_el1.exchange(0));
    if (static_cast<uint32_t>(hr) != 0)
    {
        return TranslateHaltReason(hr);
    }

    NativeExecutionParameters * thread_params = &thread->GetNativeExecutionParameters();
    IKernelProcess * process = thread->GetOwnerProcess();
    m_running_thread = thread;
    m_guest_ctx.parent = this;
    thread_params->native_context = &m_guest_ctx;
    thread_params->tpidr_el0 = m_guest_ctx.tpidr_el0;
    thread_params->tpidrro_el0 = m_guest_ctx.tpidrro_el0;
    thread_params->is_running = true;

    const uint64_t trampoline = process ? process->FindPostHandler(m_guest_ctx.pc) : 0;
    if (trampoline != 0)
    {
        hr = ReturnToRunCodeByTrampoline(thread_params, &m_guest_ctx, trampoline);
    }
    else
    {
        hr = ReturnToRunCodeByExceptionLevelChange(m_thread_id, thread_params);
    }

    m_running_thread = nullptr;
    m_guest_ctx.tpidr_el0 = thread_params->tpidr_el0;
    thread_params->native_context = nullptr;
    thread_params->is_running = false;

    return TranslateHaltReason(hr);
}

CpuHaltReason ArmNce::StepThread(IKernelThread * /*thread*/)
{
    return CpuHaltReason::StepThread;
}

uint32_t ArmNce::GetSvcNumber() const
{
    return m_guest_ctx.svc;
}

void ArmNce::GetSvcArguments(uint64_t (&args)[8]) const
{
    for (size_t i = 0; i < 8; i++)
    {
        args[i] = m_guest_ctx.cpu_registers[i];
    }
}

void ArmNce::SetSvcArguments(const uint64_t (&args)[8])
{
    for (size_t i = 0; i < 8; i++)
    {
        m_guest_ctx.cpu_registers[i] = args[i];
    }
}

ArmNce::ArmNce(ICoreSystem & system, bool /*uses_wall_clock*/, IKernelProcess & process, std::size_t core_index) :
    m_system(system),
    m_core_index(core_index)
{
    m_guest_ctx.memory = &process.GetMemory();
    m_guest_ctx.parent = this;
}

ArmNce::~ArmNce() = default;

void ArmNce::Initialize()
{
    if (m_thread_id == -1)
    {
        m_thread_id = gettid();
    }

    // Configure signal stack.
    if (!m_stack)
    {
        m_stack = std::make_unique<u8[]>(StackSize);

        stack_t ss{};
        ss.ss_sp = m_stack.get();
        ss.ss_size = StackSize;
        sigaltstack(&ss, nullptr);
    }

    // Set up signals.
    static std::once_flag flag;
    std::call_once(flag, [] {
        using HandlerType = decltype(sigaction::sa_sigaction);

        sigset_t signal_mask;
        sigemptyset(&signal_mask);
        sigaddset(&signal_mask, ReturnToRunCodeByExceptionLevelChangeSignal);
        sigaddset(&signal_mask, BreakFromRunCodeSignal);
        sigaddset(&signal_mask, GuestAlignmentFaultSignal);
        sigaddset(&signal_mask, GuestAccessFaultSignal);

        struct sigaction return_to_run_code_action{};
        return_to_run_code_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        return_to_run_code_action.sa_sigaction = reinterpret_cast<HandlerType>(&ArmNce::ReturnToRunCodeByExceptionLevelChangeSignalHandler);
        return_to_run_code_action.sa_mask = signal_mask;
        Common::SigAction(ReturnToRunCodeByExceptionLevelChangeSignal, &return_to_run_code_action,
                          nullptr);

        struct sigaction break_from_run_code_action{};
        break_from_run_code_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        break_from_run_code_action.sa_sigaction = reinterpret_cast<HandlerType>(&ArmNce::BreakFromRunCodeSignalHandler);
        break_from_run_code_action.sa_mask = signal_mask;
        Common::SigAction(BreakFromRunCodeSignal, &break_from_run_code_action, nullptr);

        struct sigaction alignment_fault_action{};
        alignment_fault_action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        alignment_fault_action.sa_sigaction = reinterpret_cast<HandlerType>(&ArmNce::GuestAlignmentFaultSignalHandler);
        alignment_fault_action.sa_mask = signal_mask;
        Common::SigAction(GuestAlignmentFaultSignal, &alignment_fault_action, nullptr);

        struct sigaction access_fault_action{};
        access_fault_action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        access_fault_action.sa_sigaction = reinterpret_cast<HandlerType>(&ArmNce::GuestAccessFaultSignalHandler);
        access_fault_action.sa_mask = signal_mask;
        Common::SigAction(GuestAccessFaultSignal, &access_fault_action, &g_orig_segv_action);
    });
}

void ArmNce::SetTpidrroEl0(u64 value)
{
    m_guest_ctx.tpidrro_el0 = value;
}

void ArmNce::GetContext(CpuThreadContext & ctx) const
{
    for (size_t i = 0; i < 29; i++)
    {
        ctx.r[i] = m_guest_ctx.cpu_registers[i];
    }
    ctx.fp = m_guest_ctx.cpu_registers[29];
    ctx.lr = m_guest_ctx.cpu_registers[30];
    ctx.sp = m_guest_ctx.sp;
    ctx.pc = m_guest_ctx.pc;
    ctx.pstate = m_guest_ctx.pstate;
    std::memcpy(ctx.v, m_guest_ctx.vector_registers.data(), sizeof(ctx.v));
    ctx.fpcr = m_guest_ctx.fpcr;
    ctx.fpsr = m_guest_ctx.fpsr;
    ctx.tpidr = m_guest_ctx.tpidr_el0;
}

void ArmNce::SetContext(const CpuThreadContext & ctx)
{
    for (size_t i = 0; i < 29; i++)
    {
        m_guest_ctx.cpu_registers[i] = ctx.r[i];
    }
    m_guest_ctx.cpu_registers[29] = ctx.fp;
    m_guest_ctx.cpu_registers[30] = ctx.lr;
    m_guest_ctx.sp = ctx.sp;
    m_guest_ctx.pc = ctx.pc;
    m_guest_ctx.pstate = ctx.pstate;
    std::memcpy(m_guest_ctx.vector_registers.data(), ctx.v, sizeof(ctx.v));
    m_guest_ctx.fpcr = ctx.fpcr;
    m_guest_ctx.fpsr = ctx.fpsr;
    m_guest_ctx.tpidr_el0 = ctx.tpidr;
}

void ArmNce::SignalInterrupt(IKernelThread * thread)
{
    m_guest_ctx.esr_el1.fetch_or((uint64_t)TranslateDynarmicHaltReason(CpuHaltReason::BreakLoop));

    NativeExecutionParameters * params = &thread->GetNativeExecutionParameters();
    LockThreadParameters(params);

    if (params->is_running)
    {
        syscall(SYS_tkill, m_thread_id, BreakFromRunCodeSignal);
    }
    else
    {
        UnlockThreadParameters(params);
    }
}

void ArmNce::ClearInstructionCache()
{
    // TODO: This is not possible to implement correctly on Linux because
    // we do not have any access to ic iallu.

    // Require accesses to complete.
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void ArmNce::InvalidateCacheRange(uint64_t /*addr*/, uint64_t /*size*/)
{
    this->ClearInstructionCache();
}

void ArmNce::Release()
{
    delete this;
}

} // namespace Core
