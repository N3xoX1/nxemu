#include "probes.h"

#include <csignal>
#include <cstdlib>

#include "darwin_ctx.h"

extern "C" char ncev_probe_load_insn[];
extern "C" char ncev_probe_store_insn[];
extern "C" char ncev_probe_ldxr_insn[];

namespace ncev {

namespace {

volatile ProbeFault g_fault;
volatile std::uint64_t g_exec_target = 0;

void ProbeHandler(int sig, siginfo_t * info, void * raw) {
    Mcontext * mc = Mctx(raw);
    const std::uint64_t pc = mc->__ss.__pc;
    const bool insn = pc == reinterpret_cast<std::uint64_t>(ncev_probe_load_insn) ||
                      pc == reinterpret_cast<std::uint64_t>(ncev_probe_store_insn) ||
                      pc == reinterpret_cast<std::uint64_t>(ncev_probe_ldxr_insn);
    const bool exec = g_exec_target != 0 && pc == g_exec_target;
    if (!insn && !exec) {
        signal(sig, SIG_DFL);
        return;
    }
    g_fault.sig = sig;
    g_fault.si_code = info->si_code;
    g_fault.si_addr = reinterpret_cast<std::uint64_t>(info->si_addr);
    g_fault.far = mc->__es.__far;
    g_fault.esr = mc->__es.__esr;
    g_fault.pc = pc;
    if (insn) {
        mc->__ss.__x[0] = 1;
        mc->__ss.__pc = pc + 4;
    } else {
        mc->__ss.__x[0] = 0xFA17;
        mc->__ss.__pc = mc->__ss.__lr;
    }
}

} // namespace

void InstallProbeHandler() {
    struct sigaction action {};
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    action.sa_sigaction = ProbeHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGBUS, &action, nullptr);
    sigaction(SIGSEGV, &action, nullptr);
}

void SetProbeExecTarget(std::uint64_t target) {
    g_exec_target = target;
}

ProbeFault LastProbeFault() {
    ProbeFault f;
    f.sig = g_fault.sig;
    f.si_code = g_fault.si_code;
    f.si_addr = g_fault.si_addr;
    f.far = g_fault.far;
    f.esr = g_fault.esr;
    f.pc = g_fault.pc;
    return f;
}

void ClearProbeFault() {
    g_fault.sig = 0;
    g_fault.si_code = 0;
    g_fault.si_addr = 0;
    g_fault.far = 0;
    g_fault.esr = 0;
    g_fault.pc = 0;
}

} // namespace ncev
