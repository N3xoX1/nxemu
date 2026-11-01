// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Darwin entry, exits and fault handling of the native code execution backend. Differences from
// Linux (arm_nce.cpp, arm_nce.s):
// - The Darwin kernel owns TPIDR_EL0, so the thread parameters are found through a TSD slot,
//   and guest code is told apart from host code by its address (the guest windows).
// - The guest is entered through SIGUSR2 raised with __pthread_kill, and the handlers work on
//   __darwin_mcontext64.
// - Host pages are 16 KiB: a fault on a page the guest may access comes from a host page shared
//   with other guest pages (see Common::NceDirectMap), and the access is emulated.

#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <signal.h>
#include <sys/ucontext.h>

#include "arm_dynarmic.h"
#include "nce/arm_nce.h"
#include <libkern/OSCacheControl.h>
#include "nce/interpreter_visitor.h"
#include "nce/read_thunk.h"
#include "nce/store_thunk.h"
#include "yuzu_common/literals.h"
#include "yuzu_common/yuzu_assert.h"

namespace Core
{

namespace
{

using namespace Common::Literals;
using Mcontext = __darwin_mcontext64;

constexpr u32 StackSize = 128_KiB;
constexpr u32 PstateNzcvMask = 0xF0000000;
constexpr u64 GuestPageSize = 0x1000;

// Sample the interpreter fallback, not every guest memory access. Fixed TLS
// storage avoids allocating in the signal handler; bounded probing may drop samples.
void ProfileReadFault(u64 pc, bool write = false) {
    static const bool enabled = std::getenv("NXEMU_NCE_FAULT_INSTRUCTIONS") != nullptr;
    if (!enabled) return;
    static thread_local u64 sequence{};
    if ((++sequence & 255) != 0) return;
    struct Site { u64 pc{}; u32 instruction{}, samples{}; bool write{}; };
    static thread_local std::array<Site, 512> sites{};
    const size_t hash = ((pc >> 2) ^ (pc >> 13)) & (sites.size() - 1);
    for (size_t probe = 0; probe < 4; ++probe) {
        auto& site = sites[(hash + probe) & (sites.size() - 1)];
        if (site.pc == 0 || site.pc == pc) {
            site.pc = pc;
            site.write = write;
            site.instruction = *reinterpret_cast<const u32*>(pc);
            ++site.samples;
            break;
        }
    }
    using Clock = std::chrono::steady_clock;
    static thread_local auto reported = Clock::now();
    const auto now = Clock::now();
    if (now - reported < std::chrono::seconds(5)) return;
    // Report the largest sampled sites without allocating or sorting a copy.
    for (unsigned rank = 0; rank < 8; ++rank) {
        auto best = sites.begin();
        for (auto it = sites.begin(); it != sites.end(); ++it)
            if (it->samples > best->samples) best = it;
        if (!best->samples) break;
        std::fprintf(stderr, "[%s] pc=%llx instruction=%08x samples=%u rate=1/256\n",
                     best->write ? "NCE-WRITE-FAULT" : "NCE-READ-FALLBACK",
                     (unsigned long long)best->pc, best->instruction, best->samples);
        best->samples = 0;
    }
    sites = {};
    reported = now;
}

struct sigaction g_orig_enter_action;
struct sigaction g_orig_break_action;
struct sigaction g_orig_bus_action;
struct sigaction g_orig_segv_action;
struct sigaction g_orig_trap_action;
struct sigaction g_orig_ill_action;

// The handlers below live in the CPU module. Restore the previous ones when the
// module is unloaded, so a later signal does not jump into unmapped code.
struct SignalHandlerRestorer
{
    bool installed = false;

    ~SignalHandlerRestorer()
    {
        if (!installed)
        {
            return;
        }
        sigaction(ReturnToRunCodeByExceptionLevelChangeSignal, &g_orig_enter_action, nullptr);
        sigaction(BreakFromRunCodeSignal, &g_orig_break_action, nullptr);
        sigaction(SIGBUS, &g_orig_bus_action, nullptr);
        sigaction(SIGSEGV, &g_orig_segv_action, nullptr);
        sigaction(SIGTRAP, &g_orig_trap_action, nullptr);
        sigaction(SIGILL, &g_orig_ill_action, nullptr);
    }
} g_signal_handler_restorer;

// Verify assembly offsets.
static_assert(offsetof(NativeExecutionParameters, native_context) == TpidrEl0NativeContext);
static_assert(offsetof(NativeExecutionParameters, lock) == TpidrEl0Lock);

// ESR_EL1 syndrome, as the Darwin kernel copies it to __es.__esr.
struct Esr
{
    u32 raw;

    u32 Ec() const
    {
        return raw >> 26;
    }
    bool IsDataAbort() const
    {
        return Ec() == 0x24 || Ec() == 0x25;
    }
    bool IsSpAlignment() const
    {
        return Ec() == 0x26;
    }
    bool IsAlignment() const
    {
        return (raw & 0x3F) == 0x21;
    }
    bool IsWrite() const
    {
        return ((raw >> 6) & 1) != 0;
    }
};

Mcontext * GetMcontext(void * raw_context)
{
    return static_cast<ucontext_t *>(raw_context)->uc_mcontext;
}

u64 & Gpr(Mcontext * mc, size_t index)
{
    if (index < 29)
    {
        return mc->__ss.__x[index];
    }
    return index == 29 ? mc->__ss.__fp : mc->__ss.__lr;
}

// Darwin stores X29/X30 outside __x[29]; use the explicit fields rather than
// indexing beyond that array when a thunk reads or restores these registers.
struct SignalRegisters {
    Mcontext* context;
    u64& operator[](size_t index) { return Gpr(context, index); }
};

NativeExecutionParameters * CurrentThreadParameters()
{
    return static_cast<NativeExecutionParameters *>(pthread_getspecific(ThreadParametersKey()));
}

void ChainSignal(const struct sigaction & previous, int sig, siginfo_t * info, void * raw_context)
{
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr)
    {
        previous.sa_sigaction(sig, info, raw_context);
        return;
    }
    if (previous.sa_handler == SIG_IGN)
    {
        return;
    }
    if (previous.sa_handler == SIG_DFL)
    {
        // Returning re-executes the faulting instruction, which then gets the default action.
        signal(sig, SIG_DFL);
        return;
    }
    previous.sa_handler(sig);
}

void InstallHandler(int sig, void (*handler)(int, siginfo_t *, void *), struct sigaction * previous)
{
    struct sigaction action{};
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    action.sa_sigaction = handler;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, ReturnToRunCodeByExceptionLevelChangeSignal);
    sigaddset(&action.sa_mask, BreakFromRunCodeSignal);
    sigaddset(&action.sa_mask, SIGBUS);
    sigaddset(&action.sa_mask, SIGSEGV);
    sigaction(sig, &action, previous);
}


} // namespace

bool ArmNce::IsGuestAddress(uint64_t addr) const
{
    for (const auto & [start, end] : m_guest_windows)
    {
        if (addr >= start && addr < end)
        {
            return true;
        }
    }
    return false;
}

void ArmNce::EnterGuestSignalHandler(int sig, siginfo_t * info, void * raw_context)
{
    NativeExecutionParameters * params = CurrentThreadParameters();
    if (params == nullptr || params->native_context == nullptr)
    {
        ChainSignal(g_orig_enter_action, sig, info, raw_context);
        return;
    }
    GuestContext * guest_ctx = static_cast<GuestContext *>(params->native_context);
    Mcontext * mc = GetMcontext(raw_context);

    // The host registers, fpcr and fpsr were saved by ReturnToRunCodeByExceptionLevelChange.
    for (size_t i = 0; i < 31; i++)
    {
        Gpr(mc, i) = guest_ctx->cpu_registers[i];
    }
    mc->__ss.__sp = guest_ctx->sp;
    mc->__ss.__pc = guest_ctx->pc;
    mc->__ss.__cpsr = (mc->__ss.__cpsr & ~PstateNzcvMask) | (guest_ctx->pstate & PstateNzcvMask);
    mc->__ns.__fpcr = guest_ctx->fpcr;
    mc->__ns.__fpsr = guest_ctx->fpsr;
    std::memcpy(mc->__ns.__v, guest_ctx->vector_registers.data(), sizeof(mc->__ns.__v));

    // Unlock the context. Returning from here will enter the guest.
    UnlockThreadParameters(params);
}

void ArmNce::SaveGuestContext(GuestContext * guest_ctx, void * raw_context)
{
    Mcontext * mc = GetMcontext(raw_context);

    for (size_t i = 0; i < 31; i++)
    {
        guest_ctx->cpu_registers[i] = Gpr(mc, i);
    }
    std::memcpy(guest_ctx->vector_registers.data(), mc->__ns.__v, sizeof(mc->__ns.__v));
    guest_ctx->fpsr = mc->__ns.__fpsr;
    guest_ctx->fpcr = mc->__ns.__fpcr;
    guest_ctx->pstate = mc->__ss.__cpsr;
    // A signal can interrupt MRS TPIDRRO_EL0 / LDR TSD between the two instructions.
    // Re-read the host TSD base on resumption: the scheduler may move this guest thread
    // to another host thread, whose TSD contains a different current context.
    guest_ctx->pc = guest_ctx->parent->m_running_thread->GetOwnerProcess()->GetNceRestartPc(mc->__ss.__pc);
    guest_ctx->sp = mc->__ss.__sp;

    // Resume the host where it entered the guest, as a return from the entry function.
    const HostContext & host_ctx = guest_ctx->host_ctx;
    mc->__ss.__sp = host_ctx.host_sp;
    for (size_t i = 0; i < 12; i++)
    {
        Gpr(mc, 19 + i) = host_ctx.host_saved_regs[i];
    }
    std::memcpy(&mc->__ns.__v[8], host_ctx.host_saved_vregs.data(), sizeof(host_ctx.host_saved_vregs));
    mc->__ns.__fpcr = host_ctx.host_fpcr;
    mc->__ns.__fpsr = host_ctx.host_fpsr;
    mc->__ss.__pc = host_ctx.host_saved_regs[11];
    mc->__ss.__x[0] = guest_ctx->esr_el1.exchange(0);
}

bool ArmNce::HandleGuestFault(ArmNce & nce, int sig, siginfo_t * /*info*/, void * raw_context)
{
    Mcontext * mc = GetMcontext(raw_context);
    if (sig == SIGTRAP) {
        const auto pc = mc->__ss.__pc;
        const auto instruction = *reinterpret_cast<const u32*>(pc);
        if (NCE::StoreThunk::IsTrap(instruction)) {
            u64 original_pc;u32 original;
            std::memcpy(&original_pc, reinterpret_cast<const void*>(pc + 4), 8);
            std::memcpy(&original, reinterpret_cast<const void*>(pc + 12), 4);
            if (!NCE::StoreThunk::Eligible(original)) return false;
            SignalRegisters registers{mc};
            NCE::StoreThunk::Complete(*nce.m_guest_ctx.memory, registers, mc->__ss.__sp, mc->__ss.__pc,
                original_pc, original, static_cast<NCE::StoreThunk::Trap>((instruction >> 5) & 0xffff), mc->__ns.__v);
            return true;
        }
        if (NCE::ReadThunk::IsTrap(instruction)) {
            // Literals follow this private trap in our own immutable patch section.
            u64 original_pc;
            u32 original_instruction;
            std::memcpy(&original_pc, reinterpret_cast<const void*>(pc + 4), sizeof(original_pc));
            std::memcpy(&original_instruction, reinterpret_cast<const void*>(pc + 12), sizeof(original_instruction));
            if (!NCE::ReadThunk::Eligible(original_instruction)) return false;
            SignalRegisters registers{mc};
            NCE::ReadThunk::Complete(*nce.m_guest_ctx.memory, registers, mc->__ss.__sp,
                                    mc->__ss.__pc, original_pc, original_instruction,
                                    ((instruction >> 5) & 0xffff) != NCE::ReadThunk::PlainTrap,
                                    ((instruction >> 5) & 0xffff) == NCE::ReadThunk::CompletedTrap, mc->__ns.__v);
            return true;
        }
        const auto restart = nce.m_running_thread->GetOwnerProcess()->GetNceRestartPc(pc);
        if (restart == pc) return false;
        const auto immediate = (instruction >> 5) & 0xffff;
        if ((instruction & 0xffe0001f) != 0xd4200000 || (immediate & ~0x3fu) != 0x7c00)
            return false;
        const auto reg = immediate & 31;
        if (reg == 31) return false;
        const auto address = Gpr(mc, reg);
        const bool write = (immediate & 0x20) != 0;
        auto& memory = *nce.m_guest_ctx.memory;
        if (!memory.NceAccessAllowed(address, write)) return false;
        if (write) {
            memory.NceTrackWrites(address);
            memory.InvalidateNCE(address & ~(GuestPageSize - 1), GuestPageSize);
        } else {
            memory.NceTrackReads(address);
        }
        mc->__ss.__pc = restart;
        return true;
    }
    const Esr esr{mc->__es.__esr};
    if (esr.IsSpAlignment())
    {
        // macOS checks stack pointer alignment and Horizon does not: a function that reserves
        // stack with sub sp, sp, xN may leave sp unaligned. Emulate its stack accesses.
        if (std::optional<u64> next_pc = MatchAndExecuteOneInstruction(*nce.m_guest_ctx.memory, mc))
        {
            mc->__ss.__pc = *next_pc;
            return true;
        }
        return false;
    }
    if (!esr.IsDataAbort())
    {
        // Instruction aborts end the guest run.
        return false;
    }

    IMemory & memory = *nce.m_guest_ctx.memory;
    if (esr.IsWrite()) ProfileReadFault(mc->__ss.__pc, true);
    const u64 fault_address = mc->__es.__far;
    const u64 fault_page = fault_address & ~(GuestPageSize - 1);

    const auto restart_pc = nce.m_running_thread->GetOwnerProcess()->GetNceRestartPc(mc->__ss.__pc);
    // A synthetic spill/restore fault must complete the original load. Its stack
    // address must never enter GPU tracking as if it were that load's address.
    if (restart_pc != mc->__ss.__pc &&
        (NCE::ReadThunk::IsTrap(*reinterpret_cast<const u32*>(restart_pc)) ||
         NCE::StoreThunk::IsTrap(*reinterpret_cast<const u32*>(restart_pc)))) {
        mc->__ss.__pc = restart_pc;
        return true;
    }
    const auto atomic_handler = nce.m_running_thread->GetOwnerProcess()->FindNceAtomicHandler(mc->__ss.__pc);
    const bool translated_atomic = atomic_handler || restart_pc != mc->__ss.__pc;
    if (translated_atomic && (esr.IsAlignment() || !memory.NceAccessAllowed(fault_address, esr.IsWrite())))
        return false;

    if (!esr.IsAlignment())
    {
        if (esr.IsWrite() && !translated_atomic &&
            nce.m_running_thread->GetOwnerProcess()->InstallNceReadHandler(mc->__ss.__pc)) {
            // Re-execute the published branch; restarting directly at its spill PC
            // would apply fault metadata before the spill has actually executed.
            return true;
        }
        // The GPU cache write-protects guest pages of the host page: make them writable and
        // retry. Their writes reach the GPU at its next synchronization.
        if (esr.IsWrite() && memory.NceTrackWrites(fault_address))
        {
            return true;
        }
        // Reads synchronize GPU data without marking an entire cached page CPU-modified.
        if (!esr.IsWrite() && memory.NceTrackReads(fault_address))
        {
            return true;
        }
        // Only writes invalidate GPU caches. Incompatible host mappings are interpreted.
        if (esr.IsWrite() && memory.InvalidateNCE(fault_page, GuestPageSize))
        {
            return true;
        }
        // The guest denies the access: skip the instruction, as the Linux backend does.
        if (!memory.NceAccessAllowed(fault_address, esr.IsWrite()))
        {
            mc->__ss.__pc += 4;
            return true;
        }
    }

    // A runtime atomic trampoline uses the canonical backing once GPU tracking has opened
    // access. Retrying its address translation preserves native LL/SC semantics; interpreting
    // just the store after a signal would have lost the hardware exclusive reservation.
    if (translated_atomic && !esr.IsAlignment()) {
        if (atomic_handler) {
            // A store can report failure without faulting when its load used another VA
            // alias. Switch both halves of every registered pair, not just this instruction.
            nce.m_running_thread->GetOwnerProcess()->EnableNceAtomicHandlers();
            mc->__ss.__pc = atomic_handler;
        } else mc->__ss.__pc = restart_pc;
        return true;
    }

    if (!translated_atomic && !esr.IsWrite() && !esr.IsAlignment()) {
        if (const auto handler = nce.m_running_thread->GetOwnerProcess()->InstallNceReadHandler(mc->__ss.__pc)) {
            mc->__ss.__pc = handler;
            return true;
        }
    }

    // The guest allows the access, which the host page refused: emulate it.
    if (!esr.IsWrite()) ProfileReadFault(mc->__ss.__pc);
    if (std::optional<u64> next_pc = MatchAndExecuteOneInstruction(memory, mc))
    {
        mc->__ss.__pc = *next_pc;
        return true;
    }

    // The interpreter does not know the instruction: skip it.
    mc->__ss.__pc += 4;
    return true;
}

void ArmNce::ExitGuestSignalHandler(int sig, siginfo_t * info, void * raw_context)
{
    Mcontext * mc = GetMcontext(raw_context);
    NativeExecutionParameters * params = CurrentThreadParameters();
    GuestContext * guest_ctx = params != nullptr ? static_cast<GuestContext *>(params->native_context) : nullptr;
    ArmNce * nce = guest_ctx != nullptr ? guest_ctx->parent : nullptr;

    if (nce == nullptr || !nce->IsGuestAddress(mc->__ss.__pc))
    {
        // Host code.
        switch (sig)
        {
        case BreakFromRunCodeSignal: return; // spurious interrupt
        case SIGBUS: ChainSignal(g_orig_bus_action, sig, info, raw_context); return;
        case SIGSEGV: ChainSignal(g_orig_segv_action, sig, info, raw_context); return;
        case SIGTRAP: ChainSignal(g_orig_trap_action, sig, info, raw_context); return;
        case SIGILL: ChainSignal(g_orig_ill_action, sig, info, raw_context); return;
        default: return;
        }
    }

    if ((sig == SIGBUS || sig == SIGSEGV || sig == SIGTRAP) && HandleGuestFault(*nce, sig, info, raw_context))
    {
        // Runtime atomic fallback instructions must redo translation after protection changes.
        mc->__ss.__pc = nce->m_running_thread->GetOwnerProcess()->GetNceRestartPc(mc->__ss.__pc);
        return;
    }

    if (sig == BreakFromRunCodeSignal)
    {
        nce->m_break_pending.store(false, std::memory_order_release);
    }
    else
    {
        // The interrupter, which holds the lock, already set BreakLoop.
        guest_ctx->esr_el1.fetch_or((uint64_t)TranslateDynarmicHaltReason(CpuHaltReason::PrefetchAbort));
        std::atomic_ref<u32>(params->lock).store(SpinLockLocked, std::memory_order_release);
    }
    SaveGuestContext(guest_ctx, raw_context);
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
    pthread_setspecific(ThreadParametersKey(), thread_params);

    const uint64_t trampoline = process ? process->FindPostHandler(m_guest_ctx.pc) : 0;
    if (trampoline != 0)
    {
        hr = ReturnToRunCodeByTrampoline(thread_params, &m_guest_ctx, trampoline);
    }
    else
    {
        hr = ReturnToRunCodeByExceptionLevelChange(m_thread_port, &m_guest_ctx);
    }

    m_running_thread = nullptr;
    m_guest_ctx.tpidr_el0 = thread_params->tpidr_el0;
    thread_params->native_context = nullptr;
    thread_params->is_running = false;
    // A break that arrived in host code was dropped as spurious.
    m_break_pending.store(false, std::memory_order_release);


    return TranslateHaltReason(hr);
}

void ArmNce::Initialize()
{
    if (m_thread_port == MACH_PORT_NULL)
    {
        m_thread = pthread_self();
        m_thread_port = pthread_mach_thread_np(m_thread);
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
        ThreadParametersKey();
        InstallHandler(ReturnToRunCodeByExceptionLevelChangeSignal, &ArmNce::EnterGuestSignalHandler, &g_orig_enter_action);
        InstallHandler(BreakFromRunCodeSignal, &ArmNce::ExitGuestSignalHandler, &g_orig_break_action);
        InstallHandler(SIGBUS, &ArmNce::ExitGuestSignalHandler, &g_orig_bus_action);
        InstallHandler(SIGSEGV, &ArmNce::ExitGuestSignalHandler, &g_orig_segv_action);
        InstallHandler(SIGTRAP, &ArmNce::ExitGuestSignalHandler, &g_orig_trap_action);
        InstallHandler(SIGILL, &ArmNce::ExitGuestSignalHandler, &g_orig_ill_action);
        g_signal_handler_restorer.installed = true;
    });
}

void ArmNce::SignalInterrupt(IKernelThread * thread)
{
    m_guest_ctx.esr_el1.fetch_or((uint64_t)TranslateDynarmicHaltReason(CpuHaltReason::BreakLoop));

    // The pending signal will stop the core and the halt reason is already set. Its sender
    // holds the lock until the core receives it, possibly after a fault that waits on the GPU.
    if (m_break_pending.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    NativeExecutionParameters * params = &thread->GetNativeExecutionParameters();
    LockThreadParameters(params);

    if (params->is_running)
    {
        pthread_kill(m_thread, BreakFromRunCodeSignal);
    }
    else
    {
        m_break_pending.store(false, std::memory_order_release);
        UnlockThreadParameters(params);
    }
}

} // namespace Core
