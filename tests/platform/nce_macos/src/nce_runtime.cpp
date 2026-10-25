#include "nce_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <libkern/OSCacheControl.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>

#include "arm_dynarmic.h"
#include "nce/patcher.h"

namespace ncev {

std::uint32_t g_params_slot_offset = 0;
std::atomic<std::uint64_t> g_spurious_interrupts{0};

namespace {

pthread_key_t g_params_key;
constexpr int MaxWindows = 16;
std::uint64_t g_window_lo[MaxWindows];
std::uint64_t g_window_hi[MaxWindows];
int g_window_count = 0;
std::uint64_t g_alloc_next = 0;
std::uint64_t g_alloc_end = 0;

struct sigaction g_prev_bus;
struct sigaction g_prev_segv;
struct sigaction g_prev_trap;
struct sigaction g_prev_ill;

constexpr std::uint32_t PstateNzcvMask = 0xF0000000u;
constexpr std::size_t SignalStackSize = 128 * 1024;

std::uint64_t ReadFpcr() {
    std::uint64_t v;
    asm volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}
std::uint64_t ReadFpsr() {
    std::uint64_t v;
    asm volatile("mrs %0, fpsr" : "=r"(v));
    return v;
}

NativeExecutionParameters * CurrentParams() {
    return static_cast<NativeExecutionParameters *>(pthread_getspecific(g_params_key));
}

void Chain(const struct sigaction & prev, int sig, siginfo_t * info, void * raw) {
    if ((prev.sa_flags & SA_SIGINFO) != 0 && prev.sa_sigaction != nullptr) {
        prev.sa_sigaction(sig, info, raw);
        return;
    }
    if (prev.sa_handler == SIG_IGN) {
        return;
    }
    if (prev.sa_handler == SIG_DFL) {
        // Re-raise with the default action once the handler returns.
        signal(sig, SIG_DFL);
        return;
    }
    prev.sa_handler(sig);
}

// ArmNce::RestoreGuestContext, Darwin frame.
void EnterHandler(int, siginfo_t *, void * raw) {
    NativeExecutionParameters * params = CurrentParams();
    if (params == nullptr || params->native_context == nullptr) {
        std::abort();
    }
    Vcpu * vcpu = static_cast<Vcpu *>(params->native_context);
    Core::GuestContext & g = vcpu->ctx;
    Mcontext * mc = Mctx(raw);

    for (int i = 0; i < 31; i++) {
        Gpr(mc, i) = g.cpu_registers[i];
    }
    mc->__ss.__sp = g.sp;
    mc->__ss.__pc = g.pc;
    mc->__ss.__cpsr = (mc->__ss.__cpsr & ~PstateNzcvMask) | (g.pstate & PstateNzcvMask);
    mc->__ns.__fpcr = g.fpcr;
    mc->__ns.__fpsr = g.fpsr;
    std::memcpy(mc->__ns.__v, g.vector_registers.data(), sizeof(mc->__ns.__v));
    vcpu->entries_signal++;

    // Last step before the guest runs, as in the Linux handler.
    UnlockParams(params);
}

// ArmNce::SaveGuestContext, Darwin frame: store the guest state and resume the host
// at the return address saved by the entry trampoline.
void SaveGuestAndReturnToHost(Vcpu * vcpu, Mcontext * mc) {
    Core::GuestContext & g = vcpu->ctx;
    for (int i = 0; i < 31; i++) {
        g.cpu_registers[i] = Gpr(mc, i);
    }
    g.sp = mc->__ss.__sp;
    g.pc = mc->__ss.__pc;
    g.pstate = mc->__ss.__cpsr;
    g.fpcr = mc->__ns.__fpcr;
    g.fpsr = mc->__ns.__fpsr;
    std::memcpy(g.vector_registers.data(), mc->__ns.__v, sizeof(mc->__ns.__v));

    const Core::HostContext & h = g.host_ctx;
    mc->__ss.__sp = h.host_sp;
    for (int i = 0; i < 10; i++) {
        mc->__ss.__x[19 + i] = h.host_saved_regs[i];
    }
    mc->__ss.__fp = h.host_saved_regs[10];
    mc->__ss.__lr = h.host_saved_regs[11];
    mc->__ss.__pc = h.host_saved_regs[11];
    std::memcpy(&mc->__ns.__v[8], h.host_saved_vregs.data(), sizeof(h.host_saved_vregs));
    if (vcpu->restore_host_fp_on_signal_exit) {
        mc->__ns.__fpcr = static_cast<std::uint32_t>(vcpu->host_fpcr);
        mc->__ns.__fpsr = static_cast<std::uint32_t>(vcpu->host_fpsr);
    }
    mc->__ss.__x[0] = g.esr_el1.exchange(0);
}

void ExitHandler(int sig, siginfo_t * info, void * raw) {
    Mcontext * mc = Mctx(raw);
    const std::uint64_t pc = mc->__ss.__pc;
    NativeExecutionParameters * params = CurrentParams();
    const bool guest = InGuestWindow(pc) && params != nullptr && params->native_context != nullptr;

    if (!guest) {
        switch (sig) {
        case SIGURG: g_spurious_interrupts++; return;
        case SIGBUS: Chain(g_prev_bus, sig, info, raw); return;
        case SIGSEGV: Chain(g_prev_segv, sig, info, raw); return;
        case SIGTRAP: Chain(g_prev_trap, sig, info, raw); return;
        case SIGILL: Chain(g_prev_ill, sig, info, raw); return;
        default: return;
        }
    }

    Vcpu * vcpu = static_cast<Vcpu *>(params->native_context);
    if ((sig == SIGBUS || sig == SIGSEGV || sig == SIGTRAP) && vcpu->fault_hook != nullptr && vcpu->fault_hook(*vcpu, sig, info, mc)) {
        return;
    }

    vcpu->exit_signal = sig;
    vcpu->exit_far = mc->__es.__far;
    vcpu->exit_esr = mc->__es.__esr;

    std::uint64_t reason = 0;
    switch (sig) {
    case SIGURG: break; // BreakLoop was set by the sender, which also holds the lock.
    case SIGTRAP: reason = HrBreakpoint(); break;
    case SIGILL: reason = HrPrefetchAbort(); break;
    default: reason = Esr{mc->__es.__esr}.IsInstructionAbort() ? HrPrefetchAbort() : HrDataAbort(); break;
    }
    if (sig != SIGURG) {
        std::atomic_ref<std::uint32_t>(params->lock).store(SpinLockLocked, std::memory_order_release);
    }
    vcpu->ctx.esr_el1.fetch_or(reason);
    SaveGuestAndReturnToHost(vcpu, mc);
}

void Install(int sig, void (*fn)(int, siginfo_t *, void *), struct sigaction * prev) {
    struct sigaction action {};
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    action.sa_sigaction = fn;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR2);
    sigaddset(&action.sa_mask, SIGURG);
    sigaddset(&action.sa_mask, SIGBUS);
    sigaddset(&action.sa_mask, SIGSEGV);
    sigaction(sig, &action, prev);
}

} // namespace

std::uint64_t HrSupervisorCall() {
    return static_cast<std::uint64_t>(TranslateDynarmicHaltReason(CpuHaltReason::SupervisorCall));
}
std::uint64_t HrBreakLoop() {
    return static_cast<std::uint64_t>(TranslateDynarmicHaltReason(CpuHaltReason::BreakLoop));
}
std::uint64_t HrBreakpoint() {
    return static_cast<std::uint64_t>(TranslateDynarmicHaltReason(CpuHaltReason::InstructionBreakpoint));
}
std::uint64_t HrDataAbort() {
    return static_cast<std::uint64_t>(TranslateDynarmicHaltReason(CpuHaltReason::DataAbort));
}
std::uint64_t HrPrefetchAbort() {
    return static_cast<std::uint64_t>(TranslateDynarmicHaltReason(CpuHaltReason::PrefetchAbort));
}

void LockParams(NativeExecutionParameters * params) {
    std::atomic_ref<std::uint32_t> lock(params->lock);
    for (;;) {
        std::uint32_t expected = SpinLockUnlocked;
        if (lock.compare_exchange_weak(expected, SpinLockLocked, std::memory_order_acquire)) {
            return;
        }
    }
}

void UnlockParams(NativeExecutionParameters * params) {
    std::atomic_ref<std::uint32_t>(params->lock).store(SpinLockUnlocked, std::memory_order_release);
}

bool InGuestWindow(std::uint64_t pc) {
    for (int i = 0; i < g_window_count; i++) {
        if (pc >= g_window_lo[i] && pc < g_window_hi[i]) {
            return true;
        }
    }
    return false;
}

void BindSignalStack() {
    stack_t current{};
    sigaltstack(nullptr, &current);
    if ((current.ss_flags & SS_DISABLE) == 0) {
        return;
    }
    stack_t ss{};
    ss.ss_sp = std::malloc(SignalStackSize);
    ss.ss_size = SignalStackSize;
    sigaltstack(&ss, nullptr);
}

void RuntimeInit(const std::vector<std::pair<std::uint64_t, std::uint64_t>> & windows) {
    for (const auto & w : windows) {
        if (g_window_count < MaxWindows) {
            g_window_lo[g_window_count] = w.first;
            g_window_hi[g_window_count] = w.second;
            g_window_count++;
        }
    }
    g_alloc_next = windows.back().first;
    g_alloc_end = windows.back().second;
    g_params_key = Core::ThreadParametersKey();
    // Darwin stores TSD slot n at TPIDRRO_EL0 + n * 8.
    g_params_slot_offset = static_cast<std::uint32_t>(g_params_key * sizeof(void *));

    struct sigaction ignored;
    Install(ReturnToRunCodeByExceptionLevelChangeSignal, EnterHandler, &ignored);
    Install(BreakFromRunCodeSignal, ExitHandler, &ignored);
    Install(SIGBUS, ExitHandler, &g_prev_bus);
    Install(SIGSEGV, ExitHandler, &g_prev_segv);
    Install(SIGTRAP, ExitHandler, &g_prev_trap);
    Install(SIGILL, ExitHandler, &g_prev_ill);
}

Vcpu::Vcpu() {
    // Run() takes the lock first, as PhysicalCore::EnterContext does.
    params.lock = SpinLockUnlocked;
}

void Vcpu::BindThread() {
    thread = pthread_self();
    port = pthread_mach_thread_np(thread);
    BindSignalStack();
}

std::uint64_t Vcpu::Run() {
    LockParams(&params);
    std::uint64_t hr = ctx.esr_el1.exchange(0);
    if (hr == 0) {
        pthread_setspecific(g_params_key, &params);
        params.native_context = this;
        params.tpidr_el0 = ctx.tpidr_el0;
        params.tpidrro_el0 = ctx.tpidrro_el0;
        std::atomic_ref<std::uint32_t>(params.is_running).store(1, std::memory_order_release);

        host_fpcr = ReadFpcr();
        host_fpsr = ReadFpsr();

        std::uint64_t trampoline = 0;
        if (post_handlers != nullptr) {
            auto it = post_handlers->find(ctx.pc);
            if (it != post_handlers->end()) {
                trampoline = it->second;
            }
        }
        if (trampoline != 0) {
            entries_trampoline++;
            hr = ncev_enter_trampoline(&params, &ctx, trampoline);
        } else {
            hr = ncev_enter_signal(port, &params, &ctx);
        }

        std::atomic_ref<std::uint32_t>(params.is_running).store(0, std::memory_order_release);
        ctx.tpidr_el0 = params.tpidr_el0;
        params.native_context = nullptr;
    }
    UnlockParams(&params);
    return hr;
}

void Vcpu::SignalInterrupt() {
    ctx.esr_el1.fetch_or(HrBreakLoop());
    LockParams(&params);
    if (std::atomic_ref<std::uint32_t>(params.is_running).load(std::memory_order_acquire) != 0) {
        pthread_kill(thread, BreakFromRunCodeSignal);
    } else {
        UnlockParams(&params);
    }
}

std::uint64_t WindowAlloc(std::uint64_t size, int prot) {
    size = (size + 0x3FFF) & ~std::uint64_t(0x3FFF);
    const std::uint64_t addr = g_alloc_next;
    // One unmapped host page between allocations catches overruns.
    g_alloc_next += size + 0x4000;
    if (g_alloc_next > g_alloc_end) {
        std::fprintf(stderr, "guest window exhausted\n");
        std::abort();
    }
    void * p = mmap(reinterpret_cast<void *>(addr), size, prot, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        std::perror("WindowAlloc mmap");
        std::abort();
    }
    return addr;
}

void WindowProtect(std::uint64_t addr, std::uint64_t size, int prot) {
    if (mprotect(reinterpret_cast<void *>(addr), size, prot) != 0) {
        std::perror("WindowProtect");
        std::abort();
    }
}

bool BuildPatchedImage(const std::vector<std::uint32_t> & code_words, GuestImage & out, std::string & error,
                       std::vector<std::uint32_t> * patched_words, bool run_patcher, bool legacy_36bit, bool legacy_36bit_sdk, std::uint64_t read_table, bool all_read_bases, bool float_reads, bool store_thunks) {
    constexpr std::uint64_t CodeOffset = 0x40; // past Patcher's ModuleCodeIndex (0x24)
    const std::uint64_t text_size = (CodeOffset + code_words.size() * 4 + 0x3FFF) & ~std::uint64_t(0x3FFF);
    std::vector<std::uint8_t> image(text_size + 64 * 1024 * 1024);
    auto * words = reinterpret_cast<std::uint32_t *>(image.data());
    for (std::uint64_t i = 0; i < text_size / 4; i++) {
        words[i] = 0xD4200000 | (0xBAD << 5); // brk #0xbad
    }
    std::memcpy(image.data() + CodeOffset, code_words.data(), code_words.size() * 4);

    std::uint64_t image_size = text_size;
    std::uint64_t patch_size = 0;
    Core::NCE::Patcher patcher;
    if (run_patcher) {
        if (!patcher.PatchText(image.data(), text_size, 0, text_size, read_table, legacy_36bit, legacy_36bit_sdk, read_table != 0, all_read_bases, {}, float_reads, store_thunks)) {
            error = "PatchText a échoué";
            return false;
        }
        patch_size = patcher.GetSectionSize();
    }
    const std::uint64_t map_size = (text_size + patch_size + 0x3FFF) & ~std::uint64_t(0x3FFF);
    const std::uint64_t base = WindowAlloc(map_size, PROT_READ | PROT_WRITE);
    patcher.SetLegacy36BitRegion(0x480000000ULL, 0xFC0000000ULL);
    out.post_handlers.clear();
    out.read_handlers.clear();
    out.restart_pcs.clear();
    if (read_table) patcher.SetAtomicTable(read_table);
    if (run_patcher) {
        if (!patcher.RelocateAndCopy(base, 0, text_size, image.data(), &image_size, &out.post_handlers, &out.restart_pcs, nullptr, &out.read_handlers)) {
            error = "RelocateAndCopy a échoué";
            return false;
        }
    }
    std::memcpy(reinterpret_cast<void *>(base), image.data(), image_size);
    WindowProtect(base, map_size, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(reinterpret_cast<void *>(base), map_size);

    out.base = base;
    out.size = map_size;
    out.code_offset = CodeOffset;
    out.patch_section_size = patch_size;
    if (patched_words != nullptr) {
        patched_words->assign(reinterpret_cast<std::uint32_t *>(image.data() + CodeOffset),
                              reinterpret_cast<std::uint32_t *>(image.data() + CodeOffset) + code_words.size());
    }
    return true;
}

void PatchImageWord(const GuestImage & image, std::uint64_t addr, std::uint32_t word) {
    WindowProtect(image.base, image.size, PROT_READ | PROT_WRITE);
    *reinterpret_cast<std::uint32_t *>(addr) = word;
    WindowProtect(image.base, image.size, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(reinterpret_cast<void *>(image.base), image.size);
}

HostFpMasks ProbeFpMasks() {
    std::uint64_t fpcr, fpsr, fpcr_all, fpsr_all;
    asm volatile("mrs %[old_c], fpcr\n"
                 "mrs %[old_s], fpsr\n"
                 "mov x9, #-1\n"
                 "msr fpcr, x9\n"
                 "msr fpsr, x9\n"
                 "mrs %[all_c], fpcr\n"
                 "mrs %[all_s], fpsr\n"
                 "msr fpcr, %[old_c]\n"
                 "msr fpsr, %[old_s]\n"
                 : [old_c] "=&r"(fpcr), [old_s] "=&r"(fpsr), [all_c] "=&r"(fpcr_all), [all_s] "=&r"(fpsr_all)
                 :
                 : "x9");
    return {static_cast<std::uint32_t>(fpcr_all), static_cast<std::uint32_t>(fpsr_all)};
}

const char * SignalName(int sig) {
    switch (sig) {
    case SIGBUS: return "SIGBUS";
    case SIGSEGV: return "SIGSEGV";
    case SIGTRAP: return "SIGTRAP";
    case SIGILL: return "SIGILL";
    case SIGURG: return "SIGURG";
    case SIGSYS: return "SIGSYS";
    case 0: return "aucun";
    default: return "autre";
    }
}

} // namespace ncev
