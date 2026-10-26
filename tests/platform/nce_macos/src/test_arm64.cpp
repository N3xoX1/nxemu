// Step 3: ARM64. x18, TPIDR*, SVC, counters, branches and atomics, with differential
// comparison of the original instruction against the code rewritten by the real
// nce/patcher.cpp.
#include <atomic>
#include <cmath>
#include <csignal>
#include <oaknut/oaknut.hpp>
#include <pthread.h>
#include <set>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "nce/instructions.h"
#include "nce/patcher.h"
#include "nce_runtime.h"
#include "probes.h"
#include "report.h"
#include "state.h"
#include "suite.h"

using namespace oaknut::util;

namespace ncev {

namespace {

constexpr std::uint32_t Brk0 = 0xD4200000;

// Busy threads above the core count force preemption of the measuring thread.
class Oversubscribe {
public:
    Oversubscribe() {
        const unsigned n = std::thread::hardware_concurrency() * 2;
        for (unsigned i = 0; i < n; i++) {
            threads_.emplace_back([this] {
                volatile std::uint64_t x = 0;
                while (!stop_.load(std::memory_order_relaxed)) {
                    x = x + 1;
                }
            });
        }
    }
    ~Oversubscribe() {
        stop_ = true;
        for (auto & t : threads_) {
            t.join();
        }
    }

private:
    std::atomic<bool> stop_{false};
    std::vector<std::thread> threads_;
};

void EmptyHandler(int) {}

std::uint64_t g_x18_written;

// Replaces the interrupted x18, as the NCE entry handler does with the guest context.
void WriteX18Handler(int, siginfo_t *, void * uc) {
    static_cast<ucontext_t *>(uc)->uc_mcontext->__ss.__x[18] = g_x18_written;
}

} // namespace

X18Survival MeasureX18() {
    constexpr std::uint64_t Value = 0x5A5A'1818'A5A5'8181ULL;
    X18Survival r;
    r.tpidr_flag = (ncev_read_tpidr() >> 48) & 1;
    {
        Oversubscribe load;
        for (int i = 0; i < 300; i++) {
            const std::uint64_t v = ncev_x18_spin(Value, 2'000'000);
            r.spin_total++;
            if (v != Value) {
                r.spin_lost++;
                r.spin_seen = v;
            }
        }
    }
    for (int i = 0; i < 1000; i++) {
        r.sys_lost += ncev_x18_syscall(Value) != Value;
    }
    const mach_port_t self = pthread_mach_thread_np(pthread_self());
    std::signal(SIGUSR1, EmptyHandler);
    for (int i = 0; i < 1000; i++) {
        r.sig_lost += ncev_x18_signal(Value, self) != Value;
    }
    struct sigaction sa {};
    sa.sa_sigaction = WriteX18Handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, nullptr);
    for (int i = 0; i < 1000; i++) {
        g_x18_written = Value ^ (0x9E37'79B9'7F4A'7C15ULL * (i + 1));
        r.mcontext_lost += ncev_x18_signal(Value, self) != g_x18_written;
    }
    std::signal(SIGUSR1, SIG_DFL);
    return r;
}

std::string X18Text(const X18Survival & r) {
    return Fmt("lost after preemption %llu/%llu (value read %#llx), after a system call %llu/1000, after a signal "
               "%llu/1000, x18 written to the signal context not applied %llu/1000; TPIDR_EL0 bit 48 = %d",
               (unsigned long long)r.spin_lost, (unsigned long long)r.spin_total, (unsigned long long)r.spin_seen,
               (unsigned long long)r.sys_lost, (unsigned long long)r.sig_lost, (unsigned long long)r.mcontext_lost,
               r.tpidr_flag);
}

namespace {

void TestX18() {
    const X18Survival r = MeasureX18();
    Expect("A3.1", r.Preserved() && r.tpidr_flag, Class::Blocker,
           "x18 kept by the kernel (executable declaring SDK " NCEV_LEGACY_SDK ")", X18Text(r),
           "Link the NxEmu main executable with -Wl,-platform_version,macos,<min>," NCEV_LEGACY_SDK
           ": XNU then keeps x18 for the whole process (bit 48 of TPIDR_EL0). Check at startup, "
           "otherwise fall back to Dynarmic");

    // Control: the same measurement in a binary that declares the current SDK.
    int status = 0;
    const std::string out =
        RunChild(g_startup.probe_dir + "/nce_macos_validate_current_sdk", {"--child", "x18"}, status);
    const bool cleared = out.find("x18 cleared") != std::string::npos;
    Record("A3.2", cleared ? Outcome::Info : Outcome::Fail, Class::Info,
           "Control: with the current SDK, the kernel clears x18",
           Fmt("%s (status %d)", out.empty() ? "no output" : out.c_str(), status));
}

void TestTpidr() {
    constexpr std::uint64_t Value = 0x0000'7EAD'BEEF'0000ULL;
    std::uint64_t lost = 0, total = 0;
    std::set<std::uint64_t> seen;
    {
        Oversubscribe load;
        for (int i = 0; i < 300; i++) {
            const std::uint64_t v = ncev_tpidr_spin(Value, 2'000'000);
            total++;
            if (v != Value) {
                lost++;
                seen.insert(v);
            }
        }
    }
    std::string values;
    for (std::uint64_t v : seen) {
        if (values.size() < 120) {
            values += Fmt("%llu ", (unsigned long long)v);
        }
    }
    int ncpu = 0;
    std::size_t len = sizeof(ncpu);
    sysctlbyname("hw.ncpu", &ncpu, &len, nullptr, 0);
    Expect("A3.3", lost == 0, Class::Change, "TPIDR_EL0 written by user space is kept",
           Fmt("overwritten %llu/%llu times; values found instead: %s(hw.ncpu=%d: CPU number)",
               (unsigned long long)lost, (unsigned long long)total, values.c_str(), ncpu),
           "Patch code and handlers must stop using TPIDR_EL0: thread parameters through "
           "TPIDRRO_EL0 + TSD slot (A3.3), and guest/host decided by the PC being in the guest window");
}

void TestTsdSlot() {
    std::atomic<std::uint64_t> bad{0};
    std::atomic<std::uint64_t> low_bits{0};
    pthread_key_t key;
    pthread_key_create(&key, nullptr);
    const std::uint32_t offset = static_cast<std::uint32_t>(key * sizeof(void *));
    {
        Oversubscribe load;
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; t++) {
            threads.emplace_back([&, t] {
                std::uint64_t marker = 0xC0DE0000ULL + t;
                pthread_setspecific(key, reinterpret_cast<void *>(marker));
                for (int i = 0; i < 2'000'000; i++) {
                    if (ncev_read_tsd_slot(offset) != marker) {
                        bad++;
                    }
                    if ((ncev_read_tpidrro() & 7) != 0) {
                        low_bits++;
                    }
                }
            });
        }
        for (auto & t : threads) {
            t.join();
        }
    }
    pthread_key_delete(key);
    Expect("A3.4", bad == 0 && low_bits == 0, Class::Blocker,
           "Thread parameters read through TPIDRRO_EL0 + TSD slot (instead of TPIDR_EL0)",
           Fmt("8 threads x 2M reads under preemption: %llu error(s), %llu TPIDRRO_EL0 value(s) with nonzero low bits; "
               "slot at byte %u (immediate ldr)",
               (unsigned long long)bad.load(), (unsigned long long)low_bits.load(), offset));
}

void TestRawSvc() {
    int status = 0;
    const std::string out = RunChild(g_startup.self_path, {"--child", "raw_svc"}, status);
    unsigned pid = 0;
    unsigned long long a = 0, b = 0, c = 0;
    std::sscanf(out.c_str(), "pid=%u svc0=%llu svc26=%llu svc7f=%llu", &pid, &a, &b, &c);
    const bool host_syscall = pid != 0 && a == pid && b == pid && c == pid;
    Expect("A3.5", !host_syscall, Class::Change, "Unpatched guest SVC trapped (and not run as a host system call)",
           Fmt("svc #0, #0x26, #0x7f with x16=SYS_getpid return %llu/%llu/%llu (pid %u): the immediate is ignored, the guest's "
               "x16 picks a real macOS system call",
               a, b, c, pid),
           "The patcher must cover every SVC before anything runs, including modules loaded later (NRO/ro), "
           "otherwise refuse NCE for the process; no unscanned guest code may run");
}

void TestCounters() {
    const std::uint64_t freq = ncev_read_cntfrq();
    Note("A3.6", "Counter frequency", Fmt("host CNTFRQ_EL0 = %llu Hz; Switch: 19,200,000 Hz", (unsigned long long)freq));

    std::uint64_t prev = ncev_read_cntvct(), backwards = 0;
    for (int i = 0; i < 1'000'000; i++) {
        const std::uint64_t v = ncev_read_cntvct();
        backwards += v < prev;
        prev = v;
    }
    Expect("A3.7", backwards == 0, Class::Blocker, "CNTVCT_EL0 readable at EL0 and monotonic",
           Fmt("1M reads, %llu step(s) backwards", (unsigned long long)backwards));

    int status = 0;
    const std::string pct = RunChild(g_startup.self_path, {"--child", "cntpct"}, status);
    Note("A3.8", "CNTPCT_EL0 at EL0 (unpatched instruction)",
         pct.find("sigill") != std::string::npos ? "SIGILL: the kernel does not expose it" : "readable: " + pct.substr(0, pct.find('\n')));

    // Real patcher on CNTVCT_EL0 and CNTFRQ_EL0 reads.
    std::vector<std::uint32_t> text(0x1000, 0xD503201F);
    text[16] = 0xD53BE040; // mrs x0, cntvct_el0
    Core::NCE::Patcher patcher;
    patcher.PatchText(reinterpret_cast<std::uint8_t *>(text.data()), text.size() * 4, 0, text.size() * 4);
    std::vector<std::uint8_t> image(text.size() * 4 + 0x100000);
    std::memcpy(image.data(), text.data(), text.size() * 4);
    std::uint64_t image_size = text.size() * 4;
    Core::NCE::EntryTrampolines trampolines;
    patcher.RelocateAndCopy(0x10000000, 0, text.size() * 4, image.data(), &image_size, &trampolines);
    const std::uint32_t after = reinterpret_cast<std::uint32_t *>(image.data())[16];
    Expect("A3.9", after != 0xD53BE040, Class::Change, "Guest CNTVCT_EL0 read converted to 19.2 MHz",
           after == 0xD53BE040 ? Fmt("instruction left as is: the game reads the host counter at %llu Hz",
                                     (unsigned long long)freq)
                               : std::string("instruction replaced"),
           "patcher: handle MRS CNTVCT_EL0 like CNTPCT_EL0 (scaled to 19.2 MHz)");

    const std::string frq = RunChild(g_startup.self_path, {"--child", "cntfrq_patch"}, status);
    const bool frq_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 && frq.find("patched") != std::string::npos;
    Expect("A3.10", frq_ok, Class::Change, "Module containing MRS CNTFRQ_EL0 accepted by the patcher",
           frq_ok ? "ok" : Fmt("the patcher stops (UNREACHABLE), status %#x", status),
           "patcher: replace UNREACHABLE with writing the constant 19,200,000 to the destination register");

}

// EL0 system instructions found in Switch games, each run in a child process.
void TestSystemInstructions() {
    struct Insn {
        const char * name;
        std::uint32_t word;
        bool patched_by_nce; // the existing patcher rewrites it
    };
    const Insn insns[] = {
        {"MRS CTR_EL0", 0xD53B0020, false},   {"MRS DCZID_EL0", 0xD53B00E0, false}, {"MRS CNTFRQ_EL0", 0xD53BE000, false},
        {"MRS CNTPCT_EL0", 0xD53BE020, true}, {"MRS CNTVCT_EL0", 0xD53BE040, false}, {"MRS TPIDRRO_EL0", 0xD53BD060, true},
        {"DC ZVA", 0xD50B7420, false},        {"DC CVAU", 0xD50B7B20, false},       {"DC CVAC", 0xD50B7A20, false},
        {"DC CIVAC", 0xD50B7E20, false},      {"IC IVAU", 0xD50B7520, false},       {"MRS MIDR_EL1", 0xD5380000, false},
    };
    std::string allowed, trapped, trapped_unpatched;
    std::uint64_t dczid = 0;
    for (const Insn & i : insns) {
        int status = 0;
        const std::string out = RunChild(g_startup.self_path, {"--child", Fmt("insn:%x", i.word)}, status);
        const bool ok = out.rfind("ok", 0) == 0;
        if (ok) {
            allowed += Fmt("%s ", i.name);
            if (i.word == 0xD53B00E0) {
                std::sscanf(out.c_str(), "ok x0=%llx", reinterpret_cast<unsigned long long *>(&dczid));
            }
        } else {
            trapped += Fmt("%s ", i.name);
            if (!i.patched_by_nce && i.word != 0xD5380000) {
                trapped_unpatched += Fmt("%s ", i.name);
            }
        }
    }
    Expect("A3.11", trapped_unpatched.empty(), Class::Change,
           "EL0 system instructions used by games run natively (or are already rewritten)",
           Fmt("allowed: %s| trapped (SIGILL): %s(MIDR_EL1 is also forbidden on Switch)", allowed.c_str(),
               trapped.c_str()),
           "patcher: emulate MRS CTR_EL0 (Cortex-A57 value 0x8444c004 or the host's, chosen explicitly): macOS refuses "
           "it at EL0 while the cache routines of games read it");
    Expect("A3.12", (dczid & 0x1F) == 4, Class::Change, "DC ZVA: same block size as the Switch (64 bytes)",
           Fmt("DCZID_EL0 = %#llx (block %u bytes, DZP=%llu); Cortex-A57: 0x4", (unsigned long long)dczid,
               4u << (dczid & 0xF), (unsigned long long)((dczid >> 4) & 1)),
           "Different DC ZVA size: emulate DCZID_EL0 reads and rewrite DC ZVA");
}

// ---- Differential: original instruction semantics vs patched code.

struct SiteResult {
    int trials = 0;
    int bad = 0;
    int x18_lost = 0;
    std::string first;
};

using Expectation = void (*)(GuestState & expected, const GuestState & in, std::uint32_t reg);

struct Differential {
    Vcpu vcpu;
    HostFpMasks fp = ProbeFpMasks();
    std::uint64_t stack = 0;
    std::mt19937_64 rng{42};

    Differential() {
        vcpu.BindThread();
        stack = WindowAlloc(256 * 1024, PROT_READ | PROT_WRITE);
    }

    // Runs one site placed before "brk #0" and compares against the model.
    SiteResult RunSite(const GuestImage & image, std::uint64_t site, std::uint32_t reg, Expectation model, int trials) {
        SiteResult r;
        vcpu.post_handlers = &image.post_handlers;
        for (int t = 0; t < trials; t++) {
            GuestState in = RandomState(rng, stack + 128 * 1024, fp);
            in.pc = site;
            LoadState(vcpu.ctx, in);
            const std::uint64_t hr = vcpu.Run();
            const GuestState got = StoreState(vcpu.ctx, true);
            GuestState expected = in;
            expected.pc = site + 4;
            model(expected, in, reg);
            DiffResult d = Diff(expected, got);
            if (hr != HrBreakpoint()) {
                d.mismatches++;
                d.text = Fmt("exit %#llx (expected brk) ", (unsigned long long)hr) + d.text;
            }
            r.trials++;
            r.x18_lost += d.x18_lost;
            if (d.mismatches != 0) {
                if (r.bad == 0) {
                    r.first = d.text;
                }
                r.bad++;
            }
        }
        return r;
    }
};

struct SiteKind {
    const char * name;
    std::uint32_t base;
    Expectation model;
};

void ModelTpidrro(GuestState & e, const GuestState & in, std::uint32_t reg) {
    e.x[reg] = in.tpidrro_el0;
}
void ModelTpidrRead(GuestState & e, const GuestState & in, std::uint32_t reg) {
    e.x[reg] = in.tpidr_el0;
}
void ModelTpidrWrite(GuestState & e, const GuestState & in, std::uint32_t reg) {
    e.tpidr_el0 = in.x[reg];
}

void TestDifferential() {
    Differential diff;
    const SiteKind kinds[] = {
        {"MRS Xn, TPIDRRO_EL0", 0xD53BD060, ModelTpidrro},
        {"MRS Xn, TPIDR_EL0", 0xD53BD040, ModelTpidrRead},
        {"MSR TPIDR_EL0, Xn", 0xD51BD040, ModelTpidrWrite},
    };
    // One module with every site, each followed by brk #0.
    std::vector<std::uint32_t> words;
    struct Placed {
        int kind;
        std::uint32_t reg;
        std::uint64_t index;
    };
    std::vector<Placed> placed;
    for (int k = 0; k < 3; k++) {
        for (std::uint32_t reg = 0; reg < 31; reg++) {
            placed.push_back({k, reg, words.size()});
            words.push_back(kinds[k].base | reg);
            words.push_back(Brk0);
        }
    }
    const std::uint64_t cntpct_index = words.size();
    const std::uint32_t cntpct_regs[] = {0, 1, 2, 3, 5, 17, 19, 29, 30};
    for (std::uint32_t reg : cntpct_regs) {
        words.push_back(0xD53BE020 | reg); // mrs xN, cntpct_el0
        words.push_back(Brk0);
    }
    const std::uint64_t excl_index = words.size();
    words.push_back(0xC85F7C01); // ldxr x1, [x0]
    words.push_back(0x91000421); // add x1, x1, #1
    words.push_back(0xC8027C01); // stxr w2, x1, [x0]
    words.push_back(Brk0);

    GuestImage image;
    std::string error;
    std::vector<std::uint32_t> patched;
    if (!BuildPatchedImage(words, image, error, &patched)) {
        Expect("A3.13", false, Class::Blocker, "Building the patched module", error);
        return;
    }
    const std::uint64_t code = image.base + image.code_offset;

    int sites_patched = 0;
    for (const Placed & p : placed) {
        sites_patched += (patched[p.index] >> 26) == 0x05; // B imm26
    }

    constexpr int Trials = 24;
    int total_sites = 0, bad_sites = 0, x18_lost = 0;
    std::string failures;
    for (const Placed & p : placed) {
        const SiteResult r = diff.RunSite(image, code + p.index * 4, p.reg, kinds[p.kind].model, Trials);
        total_sites++;
        x18_lost += r.x18_lost;
        if (r.bad != 0) {
            bad_sites++;
            if (failures.size() < 400) {
                failures += Fmt("%s x%u : %s; ", kinds[p.kind].name, p.reg, r.first.c_str());
            }
        }
    }
    Expect("A3.13", bad_sites == 0 && sites_patched == int(placed.size()), Class::Blocker,
           "Original/rewritten differential: MRS TPIDRRO_EL0, MRS/MSR TPIDR_EL0 (x0-x30)",
           Fmt("%d sites x %d random states (x0-x30, sp, NZCV, FPCR, FPSR, v0-v31): %d diverging site(s); "
               "%d/%zu sites rewritten; entry x18 lost %d times %s",
               total_sites, Trials, bad_sites, sites_patched, placed.size(), x18_lost, failures.c_str()));

    // Negative controls: the comparison must catch a wrong rewrite.
    const std::uint64_t victim_index = placed[5].index; // MRS x5, TPIDRRO_EL0
    const std::uint64_t victim = code + victim_index * 4;
    const std::uint32_t site_word = *reinterpret_cast<std::uint32_t *>(victim);
    const std::int64_t imm = static_cast<std::int64_t>(static_cast<std::int32_t>(site_word << 6) >> 6) * 4;
    const std::uint64_t handler = victim + imm;
    std::uint64_t load_addr = 0;
    const std::uint32_t load_tpidrro = 0xF9400000 | (1u << 10) | (5u << 5) | 5u; // ldr x5, [x5, #8]
    for (int i = 0; i < 8; i++) {
        if (*reinterpret_cast<std::uint32_t *>(handler + i * 4) == load_tpidrro) {
            load_addr = handler + i * 4;
        }
    }
    int caught = 0;
    if (load_addr != 0) {
        PatchImageWord(image, load_addr, 0xF9400000 | (5u << 5) | 5u); // ldr x5, [x5, #0]: wrong field
        caught += diff.RunSite(image, victim, 5, ModelTpidrro, Trials).bad == Trials;
        PatchImageWord(image, load_addr, load_tpidrro);
    }
    PatchImageWord(image, victim, 0xD503201F); // the site no longer branches to its handler
    caught += diff.RunSite(image, victim, 5, ModelTpidrro, Trials).bad == Trials;
    PatchImageWord(image, victim, site_word);
    const bool restored = diff.RunSite(image, victim, 5, ModelTpidrro, Trials).bad == 0;
    Expect("A3.14", caught == 2 && restored, Class::Blocker,
           "Negative control: a wrong rewrite is caught by the differential",
           Fmt("mutations caught %d/2 (wrong field read, branch removed), restored site correct: %s", caught,
               restored ? "yes" : "no"));

    // CNTPCT_EL0: exact registers, counter value checked against the host clock.
    int cnt_bad = 0;
    double worst_rate_error = 0;
    std::string cnt_fail;
    for (std::size_t i = 0; i < std::size(cntpct_regs); i++) {
        const std::uint32_t reg = cntpct_regs[i];
        const std::uint64_t site = code + (cntpct_index + i * 2) * 4;
        std::uint64_t first_value = 0, first_ns = 0, first_end_ns = 0;
        for (int t = 0; t < 2; t++) {
            GuestState in = RandomState(diff.rng, diff.stack + 128 * 1024, diff.fp);
            in.pc = site;
            LoadState(diff.vcpu.ctx, in);
            diff.vcpu.post_handlers = &image.post_handlers;
            const std::uint64_t ns = NowNs();
            const std::uint64_t hr = diff.vcpu.Run();
            const std::uint64_t end_ns = NowNs();
            GuestState got = StoreState(diff.vcpu.ctx, true);
            const std::uint64_t value = got.x[reg];
            GuestState expected = in;
            expected.pc = site + 4;
            expected.x[reg] = value;
            const DiffResult d = Diff(expected, got);
            if (d.mismatches != 0 || hr != HrBreakpoint()) {
                cnt_bad++;
                cnt_fail = d.text;
            }
            if (t == 0) {
                first_value = value;
                first_ns = ns;
                first_end_ns = end_ns;
                usleep(50'000);
            } else {
                const double elapsed_ticks = static_cast<double>(value - first_value);
                const double lower = double(ns - first_end_ns) * 19'200'000.0 / 1e9 - 2;
                const double upper = double(end_ns - first_ns) * 19'200'000.0 / 1e9 + 2;
                if (elapsed_ticks < lower || elapsed_ticks > upper) cnt_bad++;
                const double rate = double(value - first_value) / (double(ns - first_ns) / 1e9);
                worst_rate_error = std::max(worst_rate_error, std::fabs(rate - 19'200'000.0) / 19'200'000.0);
            }
        }
    }
    Expect("A3.15", cnt_bad == 0, Class::Blocker,
           "Rewritten MRS CNTPCT_EL0 differential: exact registers, 19.2 MHz counter bracketed by host timestamps",
           Fmt("%zu destination registers, %d divergence(s) %s; max frequency error %.4f %% (measured over 50 ms)",
               std::size(cntpct_regs), cnt_bad, cnt_fail.c_str(), worst_rate_error * 100.0));

    // Exclusive pair rewritten to the ordered form.
    const std::uint32_t ldxr_after = patched[excl_index];
    const std::uint32_t stxr_after = patched[excl_index + 2];
    const std::uint64_t mem = WindowAlloc(16 * 1024, PROT_READ | PROT_WRITE);
    int excl_bad = 0;
    std::string excl_fail;
    for (int t = 0; t < Trials; t++) {
        GuestState in = RandomState(diff.rng, diff.stack + 128 * 1024, diff.fp);
        in.pc = code + excl_index * 4;
        in.x[0] = mem + 8 * (t % 16);
        const std::uint64_t before = diff.rng();
        *reinterpret_cast<std::uint64_t *>(in.x[0]) = before;
        LoadState(diff.vcpu.ctx, in);
        const std::uint64_t hr = diff.vcpu.Run();
        GuestState got = StoreState(diff.vcpu.ctx, true);
        GuestState expected = in;
        expected.pc = in.pc + 12;
        expected.x[1] = before + 1;
        expected.x[2] = 0;
        const DiffResult d = Diff(expected, got);
        if (d.mismatches != 0 || hr != HrBreakpoint() || *reinterpret_cast<std::uint64_t *>(in.x[0]) != before + 1) {
            excl_bad++;
            excl_fail = d.text;
        }
    }
    Expect("A3.16", excl_bad == 0 && ldxr_after == 0xC85FFC01 && stxr_after == 0xC802FC01, Class::Blocker,
           "Differential of LDXR/STXR rewritten as LDAXR/STLXR",
           Fmt("ldxr -> %#x, stxr -> %#x; %d divergence(s) over %d states %s", ldxr_after, stxr_after, excl_bad, Trials,
               excl_fail.c_str()));

    // SVC: exit to the host with the full state, then return through the trampoline.
    std::vector<std::uint32_t> svc_words;
    const std::uint32_t svc_ids[] = {0x01, 0x26, 0x7F};
    for (std::uint32_t id : svc_ids) {
        svc_words.push_back(0xD4000001 | (id << 5));
        svc_words.push_back(Brk0);
    }
    GuestImage svc_image;
    BuildPatchedImage(svc_words, svc_image, error);
    const std::uint64_t svc_code = svc_image.base + svc_image.code_offset;
    int svc_bad = 0;
    std::string svc_fail;
    for (std::size_t i = 0; i < std::size(svc_ids); i++) {
        for (int t = 0; t < Trials; t++) {
            const std::uint64_t site = svc_code + i * 8;
            GuestState in = RandomState(diff.rng, diff.stack + 128 * 1024, diff.fp);
            in.pc = site;
            LoadState(diff.vcpu.ctx, in);
            diff.vcpu.post_handlers = &svc_image.post_handlers;
            const std::uint64_t hr = diff.vcpu.Run();
            GuestState at_svc = StoreState(diff.vcpu.ctx, false);
            GuestState expected = in;
            expected.pc = site + 4;
            DiffResult d = Diff(expected, at_svc);
            const bool svc_ok = hr == HrSupervisorCall() && diff.vcpu.ctx.svc == svc_ids[i];
            // Host side of the SVC: results in x0/x1.
            diff.vcpu.ctx.cpu_registers[0] = 0x1234;
            diff.vcpu.ctx.cpu_registers[1] = 0x5678;
            const std::uint64_t hr2 = diff.vcpu.Run();
            GuestState after = StoreState(diff.vcpu.ctx, true);
            expected.x[0] = 0x1234;
            expected.x[1] = 0x5678;
            DiffResult d2 = Diff(expected, after);
            if (!svc_ok || d.mismatches != 0 || hr2 != HrBreakpoint() || d2.mismatches != 0 ||
                diff.vcpu.entries_trampoline == 0) {
                svc_bad++;
                svc_fail = Fmt("exit %#llx svc=%#x %s| return %#llx %s", (unsigned long long)hr, diff.vcpu.ctx.svc,
                               d.text.c_str(), (unsigned long long)hr2, d2.text.c_str());
            }
        }
    }
    Expect("A3.17", svc_bad == 0, Class::Blocker,
           "SVC differential: exit to the host with full state, number and PC+4, then return through the trampoline",
           Fmt("%zu SVCs x %d states: %d divergence(s) %s; trampoline entries: %llu", std::size(svc_ids), Trials,
               svc_bad, svc_fail.c_str(), (unsigned long long)diff.vcpu.entries_trampoline));
}

void TestLegacyModuleScan() {
    // Frozen rtld fixture from BOTW v0, not generated by the patcher's matcher.
    const std::vector<std::uint32_t> original{
        0x320503F4, 0xB26593F8, 0x7289E9BB, 0xB000003C,
        0xF90002A9, 0xF9000519, 0xF9000535, 0xF9000328,
        0x8B180288, 0xD35BFD08, 0xF100391F, 0x54000748,
    };
    int failures = 0;
    Differential diff;
    for (int variant = 0; variant < 3; variant++) {
        auto words = original;
        if (variant == 2) words.back() ^= 4; // Same constants, different range check.
        GuestImage image;
        std::string error;
        std::vector<std::uint32_t> patched;
        if (!BuildPatchedImage(words, image, error, &patched, true, variant != 0)) {
            failures++;
            continue;
        }
        const bool relocated = variant == 1;
        failures += relocated ? patched[0] == original[0] || patched[1] == original[1]
                              : patched[0] != original[0] || patched[1] != original[1];
        const auto site = image.base + image.code_offset;
        PatchImageWord(image, site + 8, Brk0); // Execute just the two constants.
        for (int trial = 0; trial < 24; trial++) {
            auto in = RandomState(diff.rng, diff.stack + 128 * 1024, diff.fp);
            in.pc = site;
            LoadState(diff.vcpu.ctx, in);
            diff.vcpu.post_handlers = &image.post_handlers;
            const auto hr = diff.vcpu.Run();
            auto expected = in;
            expected.pc = site + 8;
            expected.x[20] = relocated ? image.base : 0x8000000;
            expected.x[24] = -expected.x[20];
            failures += hr != HrBreakpoint() || Diff(expected, StoreState(diff.vcpu.ctx, true)).mismatches != 0;
        }
    }
    Expect("A3.22", failures == 0, Class::Blocker,
           "36-bit rtld scanner: exact relocation, signature guard, 39-bit unchanged",
           Fmt("3 variants x 24 full states: %d mismatch(es); native base > 4 GiB", failures));
}

void TestLegacyAllocators() {
    const std::vector<std::uint32_t> original{
        0x91006260, 0x320503E1, 0xB25C03E2, 0x321203E3, 0x940AF05F,
        0x91010260, 0x320503E1, 0x320103E2, 0x321203E3, 0x940AF05A, 0x9101A260,
    };
    Differential diff;
    int failures = 0;
    for (int variant = 0; variant < 3; variant++) {
        for (int pair : {1, 6}) {
            auto words = original;
            if (variant == 2) words.back() ^= 4;
            GuestImage image;
            std::string error;
            std::vector<std::uint32_t> patched;
            if (!BuildPatchedImage(words, image, error, &patched, true, false, variant != 0)) {
                failures++;
                continue;
            }
            const auto site = image.base + image.code_offset + pair * 4;
            PatchImageWord(image, site + 8, Brk0);
            for (int trial = 0; trial < 24; trial++) {
                auto in = RandomState(diff.rng, diff.stack + 128 * 1024, diff.fp);
                in.pc = site;
                LoadState(diff.vcpu.ctx, in);
                diff.vcpu.post_handlers = &image.post_handlers;
                const auto hr = diff.vcpu.Run();
                auto expected = in;
                expected.pc = site + 8;
                expected.x[1] = variant == 1 ? 0x480000000ULL : 0x8000000;
                expected.x[2] = variant == 1 ? (pair == 1 ? 0xFC0000000ULL : 0x4F8000000ULL)
                                            : pair == 1 ? 0x1000000000ULL : 0x80000000ULL;
                failures += hr != HrBreakpoint() || Diff(expected, StoreState(diff.vcpu.ctx, true)).mismatches != 0;
            }
        }
    }
    Expect("A3.23", failures == 0, Class::Blocker,
           "36-bit SDK allocators: ASLR and stack bounds, signature guard, 39-bit unchanged",
           Fmt("2 pairs x 3 variants x 24 full states: %d mismatch(es)", failures));
}

// Data in .text (Absolum keeps 1.7 MiB of it there) must come out of the patcher unchanged,
// while the exclusives and SVC of the code beside it are still rewritten.
void TestDataInText() {
    constexpr std::uint32_t Ldxr = 0xC85F7C20;  // ldxr x0, [x1]
    constexpr std::uint32_t Stxr = 0xC8027C20;  // stxr w2, x0, [x1]
    constexpr std::uint32_t Svc6 = 0xD40000C1;  // svc #0x6
    constexpr std::uint32_t DataSvc = 0xD4123CE1; // "svc #0x91e7", no such SVC on Horizon
    std::vector<std::uint32_t> text(0xC00, 0xD503201F);
    text[16] = Ldxr;
    text[17] = Stxr;
    text[20] = Svc6;
    // Second 4 KiB window: words that do not decode, with instruction-like words among them.
    for (std::size_t i = 0x400; i < 0x800; ++i) {
        text[i] = 0x00010000 + static_cast<std::uint32_t>(i);
    }
    const std::size_t data_words[] = {0x410, 0x411, 0x500};
    text[0x410] = Ldxr;
    text[0x411] = Stxr;
    text[0x500] = DataSvc;
    // Last window: a few instructions, then zero padding to the end of the text. Still code.
    std::fill(text.begin() + 0x810, text.end(), 0u);
    text[0x804] = Ldxr;
    text[0x805] = Stxr;
    Core::NCE::Patcher patcher;
    patcher.PatchText(reinterpret_cast<std::uint8_t *>(text.data()), text.size() * 4, 0, text.size() * 4);
    std::vector<std::uint8_t> image(text.size() * 4 + 0x100000);
    std::memcpy(image.data(), text.data(), text.size() * 4);
    std::uint64_t image_size = text.size() * 4;
    Core::NCE::EntryTrampolines trampolines;
    patcher.RelocateAndCopy(0x10000000, 0, text.size() * 4, image.data(), &image_size, &trampolines);
    const auto * out = reinterpret_cast<const std::uint32_t *>(image.data());
    int data_changed = 0;
    for (const std::size_t i : data_words) {
        data_changed += out[i] != text[i];
    }
    const bool code_ok = out[16] == (Ldxr | (1u << 15)) && out[17] == (Stxr | (1u << 15)) && out[20] != Svc6 &&
                         out[0x804] == (Ldxr | (1u << 15)) && out[0x805] == (Stxr | (1u << 15));
    Expect("A3.24", data_changed == 0 && code_ok, Class::Blocker,
           "Data in .text left intact, exclusives and SVCs of the code rewritten",
           Fmt("%d data word(s) changed; code: ldxr %#x, stxr %#x, svc %s", data_changed, out[16], out[17],
               out[20] != Svc6 ? "rewritten" : "left"));
}

void TestBranchRange() {
    // A 64 MiB module: the first site is ~64 MiB from the patch section (PostData).
    std::vector<std::uint32_t> words(16 * 1024 * 1024, 0xD503201F);
    words[0] = 0xD53BD061; // mrs x1, tpidrro_el0
    words[1] = Brk0;
    words[words.size() / 2] = 0xD53BD062; // mrs x2, tpidrro_el0
    words[words.size() / 2 + 1] = Brk0;
    words[words.size() - 2] = 0xD53BD063; // mrs x3, tpidrro_el0
    words[words.size() - 1] = Brk0;
    GuestImage image;
    std::string error;
    const bool built = BuildPatchedImage(words, image, error);
    Differential diff;
    int bad = 0;
    const std::uint64_t code = image.base + image.code_offset;
    const std::uint64_t indices[] = {0, words.size() / 2, words.size() - 2};
    for (int i = 0; i < 3 && built; i++) {
        bad += diff.RunSite(image, code + indices[i] * 4, 1 + i, ModelTpidrro, 4).bad;
    }
    Expect("A3.18", built && bad == 0, Class::Blocker, "Branches between module and patch section (64 MiB module)",
           built ? Fmt("start, middle and end of the module: %d divergence(s); patch section %#llx bytes from the first site",
                       bad, (unsigned long long)(words.size() * 4))
                 : error);
}

void TestAtomics() {
    constexpr int Threads = 4;
    constexpr std::uint64_t PerThread = 2'000'000;
    auto run = [&](void (*fn)(std::uint64_t *, std::uint64_t), double & seconds) {
        alignas(64) static std::uint64_t counter;
        counter = 0;
        const std::uint64_t t0 = NowNs();
        std::vector<std::thread> ts;
        for (int i = 0; i < Threads; i++) {
            ts.emplace_back([&] { fn(&counter, PerThread); });
        }
        for (auto & t : ts) {
            t.join();
        }
        seconds = double(NowNs() - t0) / 1e9;
        return counter;
    };
    double s_plain = 0, s_ordered = 0;
    const std::uint64_t plain = run(ncev_excl_add, s_plain);
    const std::uint64_t ordered = run(ncev_excl_add_ordered, s_ordered);
    Expect("A3.19", plain == Threads * PerThread && ordered == Threads * PerThread, Class::Blocker,
           "Concurrent LDXR/STXR increments exact, original and ordered forms",
           Fmt("%d threads x %llu: original %llu (%.0f ms), LDAXR/STLXR %llu (%.0f ms, x%.2f)", Threads,
               (unsigned long long)PerThread, (unsigned long long)plain, s_plain * 1000, (unsigned long long)ordered,
               s_ordered * 1000, s_ordered / s_plain));

    // Message passing litmus: data then flag (exclusive store), read flag (exclusive load) then data.
    auto litmus = [](int ordered_form) {
        constexpr int Rounds = 200000;
        alignas(128) static std::uint64_t data;
        alignas(128) static std::uint64_t flag;
        std::uint64_t reordered = 0;
        std::atomic<int> round{-1};
        std::atomic<int> done{-1};
        std::thread writer([&] {
            for (int r = 0; r < Rounds; r++) {
                while (round.load(std::memory_order_acquire) != r) {
                }
                ncev_mp_write(&data, &flag, std::uint64_t(r) + 1, ordered_form);
                while (done.load(std::memory_order_acquire) != r) {
                }
            }
        });
        for (int r = 0; r < Rounds; r++) {
            data = 0;
            flag = 0;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            round.store(r, std::memory_order_release);
            for (;;) {
                std::uint64_t d = 0;
                const std::uint64_t f = ncev_mp_read(&data, &flag, &d, ordered_form);
                if (f == std::uint64_t(r) + 1) {
                    reordered += d != f;
                    break;
                }
            }
            done.store(r, std::memory_order_release);
        }
        writer.join();
        return reordered;
    };
    const std::uint64_t relaxed_bad = litmus(0);
    const std::uint64_t ordered_bad = litmus(1);
    Expect("A3.20", ordered_bad == 0, Class::Blocker,
           "Message passing with ordered exclusives (semantics AsOrdered assumes)",
           Fmt("ordered form: %llu violation(s); original form: %llu reordering(s) observed out of 200,000",
               (unsigned long long)ordered_bad, (unsigned long long)relaxed_bad));

    // Exclusive loops keep making progress under a signal storm.
    alignas(64) static std::uint64_t counter;
    counter = 0;
    std::signal(SIGUSR1, EmptyHandler);
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> sent{0};
    pthread_t target{};
    std::atomic<bool> ready{false};
    std::thread worker([&] {
        target = pthread_self();
        ready = true;
        ncev_excl_add_ordered(&counter, 20'000'000);
        stop = true;
    });
    while (!ready) {
    }
    while (!stop) {
        pthread_kill(target, SIGUSR1);
        sent++;
        usleep(20);
    }
    worker.join();
    std::signal(SIGUSR1, SIG_DFL);
    Expect("A3.21", counter == 20'000'000, Class::Blocker, "Exclusive loop under a signal storm: progress and exact result",
           Fmt("%llu increments, %llu signals received during the loop", (unsigned long long)counter,
               (unsigned long long)sent.load()));
}

} // namespace

void RunArm64Tests() {
    BeginStep("3", "ARM64");
    TestX18();
    TestTpidr();
    TestTsdSlot();
    TestRawSvc();
    TestCounters();
    TestSystemInstructions();
    TestDifferential();
    TestLegacyModuleScan();
    TestLegacyAllocators();
    TestDataInText();
    TestBranchRange();
    TestAtomics();
    EndStep();
}

} // namespace ncev
