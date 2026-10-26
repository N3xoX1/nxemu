// Step 4: transitions. Registers, stacks, guest <-> host calls, Mach exceptions and
// thread interrupts, on the Darwin runtime with the real patcher's SVC trampolines.
#include <atomic>
#include <chrono>
#include <mach/mach.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <oaknut/oaknut.hpp>
#include <random>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "nce_runtime.h"
#include "probes.h"
#include "report.h"
#include "state.h"
#include "suite.h"

using namespace oaknut::util;

namespace ncev {

namespace {

constexpr std::uint64_t KiB = 1024;

GuestImage Build(const std::vector<std::uint32_t> & words, bool patch = true) {
    GuestImage image;
    std::string error;
    if (!BuildPatchedImage(words, image, error, nullptr, patch)) {
        std::fprintf(stderr, "BuildPatchedImage: %s\n", error.c_str());
        std::abort();
    }
    return image;
}

std::uint64_t Entry(const GuestImage & image) {
    return image.base + image.code_offset;
}

void TestSignalRoundTrip() {
    std::vector<std::uint32_t> words;
    oaknut::VectorCodeGenerator c{words};
    c.BRK(0);
    const GuestImage image = Build(words, false);
    const std::uint64_t stack = WindowAlloc(64 * KiB, PROT_READ | PROT_WRITE);
    Vcpu vcpu;
    vcpu.BindThread();
    const HostFpMasks fp = ProbeFpMasks();
    std::mt19937_64 rng(7);
    constexpr int Trials = 2000;
    int bad = 0, x18_lost = 0;
    std::string first;
    std::vector<std::uint64_t> times;
    for (int t = 0; t < Trials; t++) {
        GuestState in = RandomState(rng, stack + 32 * KiB, fp);
        in.pc = Entry(image);
        LoadState(vcpu.ctx, in);
        const std::uint64_t t0 = NowNs();
        const std::uint64_t hr = vcpu.Run();
        times.push_back(NowNs() - t0);
        const GuestState got = StoreState(vcpu.ctx, true);
        const DiffResult d = Diff(in, got);
        x18_lost += d.x18_lost;
        if (d.mismatches != 0 || hr != HrBreakpoint()) {
            if (bad++ == 0) {
                first = d.text;
            }
        }
    }
    Expect("T4.1", bad == 0, Class::Blocker,
           "Entry through a signal (full context), then exit through a signal: guest state restored exactly",
           Fmt("%d random states (x0-x30, sp, pc, NZCV, FPCR, FPSR, v0-v31): %d divergence(s) %s; x18 lost %d times; "
               "round trip %s",
               Trials, bad, first.c_str(), x18_lost, StatsText(Summarize(times)).c_str()));
}

struct CalleeSavedArgs {
    Vcpu * vcpu;
    std::uint64_t svc_entry;
    std::uint64_t hr1;
    std::uint64_t hr2;
};

std::uint64_t RunSvcThenBrk(void * raw) {
    auto * a = static_cast<CalleeSavedArgs *>(raw);
    a->vcpu->ctx.pc = a->svc_entry;
    a->hr1 = a->vcpu->Run(); // exits through the patch SVC trampoline
    a->hr2 = a->vcpu->Run(); // enters through the post-SVC trampoline, exits by signal
    return 0;
}

void TestHostState() {
    std::vector<std::uint32_t> words = {0xD4000001 | (0x26 << 5), 0xD4200000}; // svc #0x26; brk #0
    const GuestImage image = Build(words);
    const std::uint64_t stack = WindowAlloc(64 * KiB, PROT_READ | PROT_WRITE);
    Vcpu vcpu;
    vcpu.BindThread();
    vcpu.post_handlers = &image.post_handlers;
    vcpu.ctx.sp = stack + 32 * KiB;
    for (int i = 0; i < 31; i++) {
        vcpu.ctx.cpu_registers[i] = 0xDEAD0000 + i; // guest values in every host callee-saved register
    }
    CalleeSavedArgs args{&vcpu, Entry(image), 0, 0};
    const std::uint64_t changed = ncev_call_check_callee_saved(RunSvcThenBrk, &args);
    Expect("T4.2", changed == 0 && args.hr1 == HrSupervisorCall() && args.hr2 == HrBreakpoint(), Class::Blocker,
           "Host registers preserved (x19-x28, d8-d15, sp) across an SVC exit and a signal exit",
           Fmt("%llu register(s) changed; exits %#llx then %#llx", (unsigned long long)changed,
               (unsigned long long)args.hr1, (unsigned long long)args.hr2));

    // Host floating-point control after the guest ran with its own FPCR.
    auto read_fpcr = [] {
        std::uint64_t v;
        asm volatile("mrs %0, fpcr" : "=r"(v));
        return v;
    };
    auto write_fpcr = [](std::uint64_t v) { asm volatile("msr fpcr, %0" ::"r"(v)); };
    const std::uint32_t guest_fpcr = 0x01C00000; // FZ, round towards zero
    write_fpcr(0);
    vcpu.ctx.fpcr = guest_fpcr;
    vcpu.ctx.pc = Entry(image);
    const std::uint64_t hr_svc = vcpu.Run();
    const std::uint64_t after_svc = read_fpcr();
    write_fpcr(0);
    const std::uint64_t hr_brk = vcpu.Run();
    const std::uint64_t after_signal = read_fpcr();
    write_fpcr(0);
    Expect("T4.3", after_signal == 0, Class::Blocker, "Host FPCR restored after a signal exit (Darwin runtime)",
           Fmt("exit %#llx, host FPCR after = %#llx", (unsigned long long)hr_brk, (unsigned long long)after_signal));
    Expect("T4.4", after_svc == 0, Class::Change, "Host FPCR restored after an SVC exit (patcher trampoline)",
           Fmt("exit %#llx, host FPCR after = %#llx (guest FPCR %#x)", (unsigned long long)hr_svc,
               (unsigned long long)after_svc, guest_fpcr),
           "SVC trampoline (patcher.cpp) and NCE signal exit: save and restore the host FPCR and FPSR, otherwise host "
           "code (HLE services, audio, GPU) runs with the game's rounding and flush-to-zero (same fault on Linux)");
}

void TestSvcLoop() {
    std::vector<std::uint32_t> words;
    oaknut::VectorCodeGenerator c{words};
    oaknut::Label loop;
    c.l(loop);
    c.SVC(0x26);
    c.SUBS(X19, X19, 1);
    c.B(oaknut::Cond::NE, loop);
    c.BRK(0);
    const GuestImage image = Build(words);
    const std::uint64_t stack = WindowAlloc(64 * KiB, PROT_READ | PROT_WRITE);
    Vcpu vcpu;
    vcpu.BindThread();
    vcpu.post_handlers = &image.post_handlers;
    constexpr std::uint64_t N = 200'000;
    vcpu.ctx.sp = stack + 32 * KiB;
    vcpu.ctx.pc = Entry(image);
    vcpu.ctx.cpu_registers[0] = 0;
    vcpu.ctx.cpu_registers[19] = N;
    vcpu.ctx.cpu_registers[25] = 0x2525;
    std::uint64_t handled = 0, other = 0;
    const std::uint64_t t0 = NowNs();
    for (;;) {
        const std::uint64_t hr = vcpu.Run();
        if (hr == HrSupervisorCall()) {
            handled += vcpu.ctx.svc == 0x26;
            vcpu.ctx.cpu_registers[0] += 1;
            continue;
        }
        if (hr != HrBreakpoint()) {
            other++;
        }
        break;
    }
    const double per = double(NowNs() - t0) / double(N);
    const bool ok = handled == N && vcpu.ctx.cpu_registers[0] == N && vcpu.ctx.cpu_registers[19] == 0 &&
                    vcpu.ctx.cpu_registers[25] == 0x2525 && other == 0;
    Expect("T4.5", ok, Class::Blocker, "Loop of 200,000 guest -> host -> guest SVCs (patcher trampolines)",
           Fmt("%llu SVCs handled, x0=%llu, trampoline entries %llu, signal %llu; %.0f ns per SVC round trip",
               (unsigned long long)handled, (unsigned long long)vcpu.ctx.cpu_registers[0],
               (unsigned long long)vcpu.entries_trampoline, (unsigned long long)vcpu.entries_signal, per));
}

// Copies of ArmNce::GetContext / ArmNce::SetContext (src/nxemu-cpu/nce/arm_nce.cpp).
void ArmNceGetContext(const Core::GuestContext & g, CpuThreadContext & ctx) {
    for (size_t i = 0; i < 29; i++) {
        ctx.r[i] = g.cpu_registers[i];
    }
    ctx.fp = g.cpu_registers[29];
    ctx.lr = g.cpu_registers[30];
    ctx.sp = g.sp;
    ctx.pc = g.pc;
    ctx.pstate = g.pstate;
    std::memcpy(ctx.v, g.vector_registers.data(), sizeof(ctx.v));
    ctx.fpcr = g.fpcr;
    ctx.fpsr = g.fpsr;
    ctx.tpidr = g.tpidr_el0;
}
void ArmNceSetContext(Core::GuestContext & g, const CpuThreadContext & ctx) {
    for (size_t i = 0; i < 29; i++) {
        g.cpu_registers[i] = ctx.r[i];
    }
    g.cpu_registers[29] = ctx.fp;
    g.cpu_registers[30] = ctx.lr;
    g.sp = ctx.sp;
    g.pc = ctx.pc;
    g.pstate = ctx.pstate;
    std::memcpy(g.vector_registers.data(), ctx.v, sizeof(ctx.v));
    g.fpcr = ctx.fpcr;
    g.fpsr = ctx.fpsr;
    g.tpidr_el0 = ctx.tpidr;
}

void TestFlagsAcrossMigration() {
    // svc; b.eq ok; brk #1; ok: brk #0. The guest enters with Z set.
    std::vector<std::uint32_t> words;
    oaknut::VectorCodeGenerator c{words};
    oaknut::Label ok;
    c.SVC(0x26);
    c.B(oaknut::Cond::EQ, ok);
    c.BRK(1);
    c.l(ok);
    c.BRK(0);
    const GuestImage image = Build(words);
    const std::uint64_t stack = WindowAlloc(64 * KiB, PROT_READ | PROT_WRITE);
    const std::uint64_t ok_pc = Entry(image) + 12;

    auto run = [&](bool migrate) {
        Vcpu core_a, core_b;
        core_a.BindThread();
        core_b.BindThread();
        core_a.post_handlers = core_b.post_handlers = &image.post_handlers;
        core_a.ctx.sp = stack + 32 * KiB;
        core_a.ctx.pc = Entry(image);
        core_a.ctx.pstate = 0x40000000; // Z
        core_a.Run();                                      // SVC exit
        Vcpu & resume = migrate ? core_b : core_a;
        if (migrate) {
            // The kernel saves the thread on core A and loads it on core B, whose last
            // SVC left other flags in its GuestContext.
            CpuThreadContext saved{};
            ArmNceGetContext(core_a.ctx, saved);
            core_b.ctx.pstate = 0;
            ArmNceSetContext(core_b.ctx, saved);
        }
        resume.Run();
        return resume.ctx.pc == ok_pc;
    };
    const bool same_core = run(false);
    const bool migrated = run(true);
    Expect("T4.6", same_core, Class::Blocker, "NZCV flags kept across an SVC (same core)",
           same_core ? "b.eq taken after the return" : "flags lost");
    Expect("T4.7", migrated, Class::Change, "NZCV flags kept when the thread resumes on another core after an SVC",
           migrated ? "b.eq taken" : "b.eq not taken: the SVC exit writes GuestContext::nzcv, GetContext/SetContext only "
                                    "copy pstate, the trampoline reloads the core's nzcv",
           "GuestContext: a single flags field (SVC exit, signal exit, Get/SetContext and LoadContext); "
           "the fault exists on Linux too");
}

void TestStacksAndFaults() {
    // Guest stack of 16 KiB with an unmapped guard page below it.
    const std::uint64_t stack = WindowAlloc(16 * KiB, PROT_READ | PROT_WRITE);
    std::vector<std::uint32_t> words;
    oaknut::VectorCodeGenerator c{words};
    oaknut::Label loop;
    c.l(loop);
    c.SVC(0x26);
    c.SUBS(X19, X19, 1);
    c.B(oaknut::Cond::NE, loop);
    c.BRK(0);
    const GuestImage image = Build(words);
    Vcpu vcpu;
    vcpu.BindThread();
    vcpu.post_handlers = &image.post_handlers;
    vcpu.ctx.sp = stack + 64; // 64 bytes above the guard page
    vcpu.ctx.pc = Entry(image);
    vcpu.ctx.cpu_registers[19] = 1000;
    std::uint64_t hr = 0, svcs = 0;
    do {
        hr = vcpu.Run();
        svcs += hr == HrSupervisorCall();
    } while (hr == HrSupervisorCall());
    Expect("T4.8", hr == HrBreakpoint() && svcs == 1000 && vcpu.ctx.sp == stack + 64, Class::Blocker,
           "Minimal guest stack (64 bytes above a guard page): no access outside the stack",
           Fmt("1000 SVCs, final exit %#llx, final sp +%llu; handlers on the alternate stack", (unsigned long long)hr,
               (unsigned long long)(vcpu.ctx.sp - stack)));

    // Guest faults: exact state and address.
    words.clear();
    oaknut::VectorCodeGenerator f{words};
    f.STR(X1, X0);
    f.BRK(0);
    const GuestImage fault_image = Build(words, false);
    const std::uint64_t none = WindowAlloc(16 * KiB, PROT_NONE);
    const HostFpMasks fp = ProbeFpMasks();
    std::mt19937_64 rng(9);
    int bad = 0;
    std::string first;
    const std::uint64_t targets[] = {none + 0x1238, 0x7F00'0000'1000ULL};
    std::string sigs;
    for (std::uint64_t target : targets) {
        GuestState in = RandomState(rng, stack + 8 * KiB, fp);
        in.pc = Entry(fault_image);
        in.x[0] = target;
        LoadState(vcpu.ctx, in);
        hr = vcpu.Run();
        const GuestState got = StoreState(vcpu.ctx, true);
        const DiffResult d = Diff(in, got);
        sigs += Fmt("%s far=%#llx ", SignalName(vcpu.exit_signal), (unsigned long long)vcpu.exit_far);
        if (hr != HrDataAbort() || d.mismatches != 0 || vcpu.exit_far != target) {
            if (bad++ == 0) {
                first = d.text;
            }
        }
    }
    Expect("T4.9", bad == 0, Class::Blocker, "Guest access fault: DataAbort exit, exact state and address",
           Fmt("PROT_NONE page of the window and address outside the window: %s; %d divergence(s) %s", sigs.c_str(), bad,
               first.c_str()));

    ClearProbeFault();
    std::uint64_t dummy = 0;
    const int host_fault = ncev_probe_load(reinterpret_cast<void *>(none + 8), &dummy);
    Expect("T4.10", host_fault == 1 && LastProbeFault().sig != 0, Class::Blocker,
           "Fault in host code passed to the previous handler (chaining)",
           Fmt("host read of a PROT_NONE guest page: %s", SignalName(LastProbeFault().sig)));
}

// Task-level EXC_BAD_ACCESS port that declines every exception, as Dynarmic's macOS
// handler does for faults outside its JIT code.
class DecliningMachPort {
public:
    void Start() {
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port_);
        mach_port_insert_right(mach_task_self(), port_, port_, MACH_MSG_TYPE_MAKE_SEND);
        old_count_ = EXC_TYPES_COUNT;
        task_get_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, old_masks_, &old_count_, old_ports_,
                                 old_behaviors_, old_flavors_);
        task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, port_, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,
                                 THREAD_STATE_NONE);
        thread_ = std::thread([this] { Loop(); });
    }
    void Stop() {
        task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
        for (mach_msg_type_number_t i = 0; i < old_count_; i++) {
            task_set_exception_ports(mach_task_self(), old_masks_[i], old_ports_[i], old_behaviors_[i], old_flavors_[i]);
        }
        stop_ = true;
        thread_.join();
        mach_port_mod_refs(mach_task_self(), port_, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), port_);
    }
    std::uint64_t received() const { return received_; }

private:
    void Loop() {
        while (!stop_) {
            union {
                mach_msg_header_t head;
                char buffer[2048];
            } msg;
            const kern_return_t kr =
                mach_msg(&msg.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(msg), port_, 50, MACH_PORT_NULL);
            if (kr != KERN_SUCCESS) {
                continue;
            }
            received_++;
            mig_reply_error_t reply{};
            reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(msg.head.msgh_bits), 0);
            reply.Head.msgh_remote_port = msg.head.msgh_remote_port;
            reply.Head.msgh_local_port = MACH_PORT_NULL;
            reply.Head.msgh_size = sizeof(reply);
            reply.Head.msgh_id = msg.head.msgh_id + 100;
            reply.NDR = NDR_record;
            reply.RetCode = KERN_FAILURE;
            mach_msg(&reply.Head, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
            msg.head.msgh_remote_port = MACH_PORT_NULL;
            mach_msg_destroy(&msg.head);
        }
    }

    mach_port_t port_ = MACH_PORT_NULL;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> received_{0};
    exception_mask_t old_masks_[EXC_TYPES_COUNT];
    mach_msg_type_number_t old_count_ = 0;
    mach_port_t old_ports_[EXC_TYPES_COUNT];
    exception_behavior_t old_behaviors_[EXC_TYPES_COUNT];
    thread_state_flavor_t old_flavors_[EXC_TYPES_COUNT];
};

void TestMachExceptions() {
    std::vector<std::uint32_t> words;
    oaknut::VectorCodeGenerator c{words};
    c.STR(X1, X0);
    c.BRK(0);
    const GuestImage image = Build(words, false);
    const std::uint64_t none = WindowAlloc(16 * KiB, PROT_NONE);
    const std::uint64_t stack = WindowAlloc(64 * KiB, PROT_READ | PROT_WRITE);
    Vcpu vcpu;
    vcpu.BindThread();
    auto measure = [&](int n, int & bad) {
        std::vector<std::uint64_t> times;
        for (int i = 0; i < n; i++) {
            vcpu.ctx.sp = stack + 32 * KiB;
            vcpu.ctx.pc = Entry(image);
            vcpu.ctx.cpu_registers[0] = none + 16;
            const std::uint64_t t0 = NowNs();
            const std::uint64_t hr = vcpu.Run();
            times.push_back(NowNs() - t0);
            bad += hr != HrDataAbort() || vcpu.ctx.pc != Entry(image);
        }
        return Summarize(times);
    };
    int bad_plain = 0, bad_mach = 0;
    const Stats plain = measure(2000, bad_plain);
    DecliningMachPort port;
    port.Start();
    const Stats with_port = measure(2000, bad_mach);
    port.Stop();
    Expect("T4.11", bad_plain == 0 && bad_mach == 0 && port.received() >= 2000, Class::Blocker,
           "Mach exception port (as Dynarmic uses) that declines: the NCE BSD signal is still delivered",
           Fmt("%llu Mach exceptions received then declined, %d wrong exit(s)", (unsigned long long)port.received(),
               bad_plain + bad_mach));
    Note("T4.12", "Cost of a guest fault until the return to the host",
         "signal only: " + StatsText(plain) + "; with a declining Mach port: " + StatsText(with_port));
}

void TestInterrupts() {
    std::vector<std::uint32_t> words;
    oaknut::VectorCodeGenerator c{words};
    oaknut::Label loop;
    c.l(loop);
    c.ADD(X0, X0, 1);
    c.B(loop);
    const GuestImage image = Build(words, false);
    const std::uint64_t stack = WindowAlloc(64 * KiB, PROT_READ | PROT_WRITE);
    constexpr int N = 3000;
    Vcpu vcpu;
    std::atomic<int> exits{0};
    std::atomic<bool> bound{false};
    std::vector<std::uint64_t> sent(N), received(N);
    int bad = 0;
    std::string first;
    std::thread guest([&] {
        vcpu.BindThread();
        vcpu.ctx.sp = stack + 32 * KiB;
        vcpu.ctx.pc = Entry(image);
        for (int i = 1; i < 31; i++) {
            vcpu.ctx.cpu_registers[i] = 0x1000 + i;
        }
        bound = true;
        std::uint64_t last_x0 = 0;
        for (int i = 0; i < N; i++) {
            const std::uint64_t hr = vcpu.Run();
            received[i] = NowNs();
            bool ok = hr == HrBreakLoop() && vcpu.ctx.cpu_registers[0] >= last_x0 &&
                      (vcpu.ctx.pc == Entry(image) || vcpu.ctx.pc == Entry(image) + 4);
            for (int r = 1; r < 31; r++) {
                ok = ok && vcpu.ctx.cpu_registers[r] == std::uint64_t(0x1000 + r);
            }
            if (!ok && bad++ == 0) {
                first = Fmt("exit %#llx pc=%#llx", (unsigned long long)hr, (unsigned long long)vcpu.ctx.pc);
            }
            last_x0 = vcpu.ctx.cpu_registers[0];
            exits = i + 1;
        }
    });
    while (!bound) {
    }
    for (int i = 0; i < N; i++) {
        usleep(200);
        sent[i] = NowNs();
        vcpu.SignalInterrupt();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (exits.load() < i + 1 && std::chrono::steady_clock::now() < deadline) {
        }
    }
    guest.join();
    std::vector<std::uint64_t> latency;
    for (int i = 0; i < N; i++) {
        latency.push_back(received[i] > sent[i] ? received[i] - sent[i] : 0);
    }
    Expect("T4.13", bad == 0 && exits == N, Class::Blocker,
           "Interrupting a guest thread in an infinite loop (targeted SIGURG), then exact resume",
           Fmt("%d interrupts, %d wrong exit(s) %s; latency request -> host return %s", N, bad, first.c_str(),
               StatsText(Summarize(latency)).c_str()));
}

void TestInterruptSvcRace() {
    std::vector<std::uint32_t> words;
    oaknut::VectorCodeGenerator c{words};
    oaknut::Label loop;
    c.l(loop);
    c.SVC(0x26);
    c.ADD(X1, X1, 1);
    c.CMP(X1, X2);
    c.B(oaknut::Cond::NE, loop);
    c.BRK(0);
    const GuestImage image = Build(words);
    const std::uint64_t stack = WindowAlloc(64 * KiB, PROT_READ | PROT_WRITE);
    constexpr std::uint64_t N = 20'000'000;
    Vcpu vcpu;
    std::atomic<bool> done{false}, bound{false};
    std::uint64_t svcs = 0, breaks = 0, bad_svc = 0, unexpected = 0;
    bool timeout = false;
    std::thread guest([&] {
        vcpu.BindThread();
        vcpu.post_handlers = &image.post_handlers;
        vcpu.ctx.sp = stack + 32 * KiB;
        vcpu.ctx.pc = Entry(image);
        vcpu.ctx.cpu_registers[0] = 0;
        vcpu.ctx.cpu_registers[1] = 0;
        vcpu.ctx.cpu_registers[2] = N;
        bound = true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        for (;;) {
            const std::uint64_t hr = vcpu.Run();
            if (hr & HrSupervisorCall()) {
                svcs++;
                bad_svc += vcpu.ctx.svc != 0x26;
                vcpu.ctx.cpu_registers[0] += 1;
            }
            if (hr & HrBreakLoop()) {
                breaks++;
            }
            if (hr & HrBreakpoint()) {
                break;
            }
            if ((hr & ~(HrSupervisorCall() | HrBreakLoop())) != 0) {
                unexpected++;
                break;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                timeout = true;
                break;
            }
        }
        done = true;
    });
    while (!bound) {
    }
    std::mt19937 rng(3);
    std::uint64_t sent = 0;
    while (!done) {
        vcpu.SignalInterrupt();
        sent++;
        usleep(rng() % 40);
    }
    guest.join();
    const bool ok = !timeout && unexpected == 0 && bad_svc == 0 && svcs == N && vcpu.ctx.cpu_registers[0] == N &&
                    vcpu.ctx.cpu_registers[1] == N;
    Expect("T4.14", ok, Class::Blocker, "Random interrupts during 20 M SVCs: no loss, no duplicate, no hang",
           Fmt("%llu SVCs (expected %llu), x0=%llu x1=%llu, %llu interrupts sent / %llu BreakLoop exits, "
               "%llu unexpected exit(s)%s",
               (unsigned long long)svcs, (unsigned long long)N, (unsigned long long)vcpu.ctx.cpu_registers[0],
               (unsigned long long)vcpu.ctx.cpu_registers[1], (unsigned long long)sent, (unsigned long long)breaks,
               (unsigned long long)unexpected, timeout ? " ; BLOCAGE" : ""));
}

void TestSpuriousInterrupts() {
    const std::uint64_t before = g_spurious_interrupts.load();
    std::atomic<bool> stop{false}, ready{false};
    pthread_t target{};
    std::thread host([&] {
        BindSignalStack();
        target = pthread_self();
        ready = true;
        volatile std::uint64_t x = 0;
        while (!stop) {
            x = x + 1;
        }
    });
    while (!ready) {
    }
    constexpr int N = 10000;
    for (int i = 0; i < N; i++) {
        pthread_kill(target, SIGURG);
        if (i % 64 == 0) {
            usleep(10);
        }
    }
    usleep(20000);
    stop = true;
    host.join();
    const std::uint64_t seen = g_spurious_interrupts.load() - before;
    Expect("T4.15", seen > 0, Class::Blocker, "SIGURG received in host code is ignored without effect",
           Fmt("%d sent, %llu received (coalesced by the kernel) and ignored, host thread intact", N, (unsigned long long)seen));
}

void TestExclusiveUnderInterrupts() {
    std::vector<std::uint32_t> words;
    oaknut::VectorCodeGenerator c{words};
    oaknut::Label loop;
    c.l(loop);
    c.LDAXR(X1, X0);
    c.ADD(X1, X1, 1);
    c.STLXR(W2, X1, X0);
    c.CBNZ(W2, loop);
    c.SUBS(X3, X3, 1);
    c.B(oaknut::Cond::NE, loop);
    c.BRK(0);
    const GuestImage image = Build(words);
    const std::uint64_t mem = WindowAlloc(16 * KiB, PROT_READ | PROT_WRITE);
    *reinterpret_cast<std::uint64_t *>(mem) = 0;
    constexpr std::uint64_t N = 3'000'000;
    Vcpu cores[2];
    std::atomic<int> finished{0}, bound{0};
    std::uint64_t breaks[2] = {0, 0}, last_hr[2] = {0, 0};
    std::vector<std::thread> threads;
    for (int k = 0; k < 2; k++) {
        threads.emplace_back([&, k] {
            const std::uint64_t stack = WindowAlloc(64 * KiB, PROT_READ | PROT_WRITE);
            Vcpu & v = cores[k];
            v.BindThread();
            v.post_handlers = &image.post_handlers;
            v.ctx.sp = stack + 32 * KiB;
            v.ctx.pc = Entry(image);
            v.ctx.cpu_registers[0] = mem;
            v.ctx.cpu_registers[3] = N;
            bound++;
            for (;;) {
                const std::uint64_t hr = v.Run();
                if (hr == 0 || hr == HrBreakLoop()) {
                    breaks[k]++;
                    continue;
                }
                last_hr[k] = hr;
                break;
            }
            finished++;
        });
        while (bound.load() <= k) {
        }
    }
    while (finished.load() < 2) {
        cores[0].SignalInterrupt();
        cores[1].SignalInterrupt();
        usleep(30);
    }
    for (auto & t : threads) {
        t.join();
    }
    const std::uint64_t total = *reinterpret_cast<std::uint64_t *>(mem);
    for (int k = 0; k < 2; k++) {
        std::printf("T4.16 core %d hr=%#llx pc=%#llx signal=%d far=%#llx remaining=%llu status=%llu\n", k,
                    (unsigned long long)last_hr[k], (unsigned long long)cores[k].ctx.pc,
                    cores[k].exit_signal, (unsigned long long)cores[k].exit_far,
                    (unsigned long long)cores[k].ctx.cpu_registers[3],
                    (unsigned long long)cores[k].ctx.cpu_registers[2]);
    }
    Expect("T4.16", total == 2 * N, Class::Blocker, "Guest exclusives on 2 cores under an interrupt storm",
           Fmt("counter %llu (expected %llu), interrupts handled %llu + %llu", (unsigned long long)total,
               (unsigned long long)(2 * N), (unsigned long long)breaks[0], (unsigned long long)breaks[1]));
}

} // namespace

void RunTransitionTests() {
    BeginStep("4", "Transitions");
    TestSignalRoundTrip();
    TestHostState();
    TestSvcLoop();
    TestFlagsAcrossMigration();
    TestStacksAndFaults();
    TestMachExceptions();
    TestInterrupts();
    TestInterruptSvcRace();
    TestSpuriousInterrupts();
    TestExclusiveUnderInterrupts();
    EndStep();
}

} // namespace ncev
