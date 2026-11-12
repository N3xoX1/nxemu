// Differential test of the A64 JIT: random instruction sequences run on two JITs whose configs
// differ only by UserConfig::test_variant; registers, flags, FPSR and memory writes must match.
// Called from a script through the exported nxemu_jit_fuzz (no emulator needed).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "dynarmic/frontend/A64/decoder/a64.h"
#include "dynarmic/frontend/A64/translate/impl/impl.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"

namespace {

using namespace Dynarmic;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u64 code_base = 0x10000;
constexpr u32 svc0 = 0xD4000001;
constexpr size_t arena_size = 1 << 20;  // fastmem mode: guest addresses below 1 MiB

struct Pattern {
    const char* fn;
    const char* name;
    u32 mask;
    u32 expected;
};

std::vector<Pattern> MakePatterns() {
    struct Raw {
        const char* fn;
        const char* name;
        const char* bits;
    };
    static const Raw raw[] = {
#define INST(fn, name, bitstring) {#fn, name, bitstring},
#include "dynarmic/frontend/A64/decoder/a64.inc"
#undef INST
    };
    // Instructions that leave the sequence, talk to the system or need the exclusive monitor
    static const std::unordered_set<std::string> excluded{
        "SVC", "BRK", "HLT", "MSR_imm", "MSR_reg", "MRS", "SYS", "SYSL", "HINT", "WFE", "WFI", "YIELD", "SEV", "SEVL",
        "CLREX", "DSB", "DMB", "ISB", "BLR", "BR", "ERET", "RET", "BL", "BLRA", "BRA", "ERETA", "RETA",
        "STXR", "STLXR", "STXP", "STLXP", "LDXR", "LDAXR", "LDXP", "LDAXP",
        "DC_IVAC", "DC_ISW", "DC_CSW", "DC_CISW", "DC_ZVA", "DC_CVAC", "DC_CVAU", "DC_CVAP", "DC_CIVAC",
        "IC_IALLU", "IC_IALLUIS", "IC_IVAU"};
    std::vector<Pattern> out;
    for (const Raw& r : raw) {
        if (excluded.count(r.fn) || std::strncmp(r.fn, "UnallocatedEncoding", 19) == 0) {
            continue;
        }
        u32 mask = 0, expected = 0;
        for (int i = 0; i < 32; i++) {
            const u32 bit = 1u << (31 - i);
            if (r.bits[i] == '0') {
                mask |= bit;
            } else if (r.bits[i] == '1') {
                mask |= bit;
                expected |= bit;
            }
        }
        out.push_back({r.fn, r.name, mask, expected});
    }
    return out;
}

// Points a branch forward by k instructions (k >= 1), so every sequence reaches its final SVC
u32 ForwardBranch(const char* fn, u32 inst, u32 k) {
    if (!std::strcmp(fn, "B_cond") || !std::strcmp(fn, "CBZ") || !std::strcmp(fn, "CBNZ")) {
        return (inst & ~(0x7FFFFu << 5)) | (k << 5);
    }
    if (!std::strcmp(fn, "TBZ") || !std::strcmp(fn, "TBNZ")) {
        return (inst & ~(0x3FFFu << 5)) | (k << 5);
    }
    if (!std::strcmp(fn, "B_uncond")) {
        return (inst & ~0x3FFFFFFu) | k;
    }
    return inst;
}

bool IsBranch(const char* fn) {
    return !std::strcmp(fn, "B_cond") || !std::strcmp(fn, "CBZ") || !std::strcmp(fn, "CBNZ") || !std::strcmp(fn, "TBZ") ||
           !std::strcmp(fn, "TBNZ") || !std::strcmp(fn, "B_uncond");
}

u8 InitialByte(u64 addr) {
    u64 x = addr * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    return static_cast<u8>(x * 0xBF58476D1CE4E5B9ull >> 56);
}

struct Env final : A64::UserCallbacks {
    std::vector<u32> code;
    std::vector<const char*> names;
    std::map<u64, u8> writes;
    u8* arena = nullptr;
    A64::Jit* jit = nullptr;
    bool bad = false;  // exception or interpreter fallback: the sequence is skipped
    bool done = false;  // reached the final SVC or was skipped
    std::map<std::string, int>* reasons = nullptr;
    void Skip(const std::string& why) {
        if (!bad && reasons) {
            (*reasons)[why]++;
        }
        bad = true;
        done = true;
        jit->HaltExecution();
    }

    u8 Read(u64 a) {
        if (arena && a < arena_size) {
            return arena[a];
        }
        const auto it = writes.find(a);
        return it != writes.end() ? it->second : InitialByte(a);
    }
    void Write(u64 a, u8 v) {
        if (arena && a < arena_size) {
            arena[a] = v;
        } else {
            writes[a] = v;
        }
    }
    template<typename T>
    T ReadN(u64 a) {
        T v = 0;
        for (size_t i = 0; i < sizeof(T); i++) {
            v |= static_cast<T>(Read(a + i)) << (8 * i);
        }
        return v;
    }
    template<typename T>
    void WriteN(u64 a, T v) {
        for (size_t i = 0; i < sizeof(T); i++) {
            Write(a + i, static_cast<u8>(v >> (8 * i)));
        }
    }

    std::optional<u32> MemoryReadCode(u64 a) override {
        if (a >= code_base && a < code_base + 4 * code.size()) {
            return code[(a - code_base) / 4];
        }
        return svc0;
    }
    u8 MemoryRead8(u64 a) override { return Read(a); }
    std::uint16_t MemoryRead16(u64 a) override { return ReadN<std::uint16_t>(a); }
    u32 MemoryRead32(u64 a) override { return ReadN<u32>(a); }
    u64 MemoryRead64(u64 a) override { return ReadN<u64>(a); }
    A64::Vector MemoryRead128(u64 a) override { return {ReadN<u64>(a), ReadN<u64>(a + 8)}; }
    void MemoryWrite8(u64 a, u8 v) override { Write(a, v); }
    void MemoryWrite16(u64 a, std::uint16_t v) override { WriteN(a, v); }
    void MemoryWrite32(u64 a, u32 v) override { WriteN(a, v); }
    void MemoryWrite64(u64 a, u64 v) override { WriteN(a, v); }
    void MemoryWrite128(u64 a, A64::Vector v) override {
        WriteN(a, v[0]);
        WriteN(a + 8, v[1]);
    }
    void InterpreterFallback(u64 pc, size_t) override {
        const size_t i = (pc - code_base) / 4;
        Skip(std::string("fallback ") + (i < names.size() ? names[i] : "?"));
    }
    void CallSVC(u32) override {
        done = true;
        jit->HaltExecution();
    }
    void ExceptionRaised(u64 pc, A64::Exception e) override {
        const size_t i = (pc - code_base) / 4;
        Skip("exception " + std::to_string(static_cast<int>(e)) + " " + (i < names.size() ? names[i] : "?"));
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1000000; }
    u64 GetCNTPCT() override { return 0; }
};

struct Runner {
    Env env;
    u64 tpidr = 0, tpidrro = 0;
    std::unique_ptr<A64::Jit> jit;
    std::vector<u8> arena_storage;
    u8* arena = nullptr;

    Runner(u32 variant, bool fastmem) {
        A64::UserConfig conf;
        conf.callbacks = &env;
        conf.tpidr_el0 = &tpidr;
        conf.tpidrro_el0 = &tpidrro;
        conf.dczid_el0 = 4;
        conf.ctr_el0 = 0x8444c004;
        conf.cntfrq_el0 = 19200000;
        conf.define_unpredictable_behaviour = true;
        conf.enable_cycle_counting = false;
        conf.code_cache_size = 64 * 1024 * 1024;
        conf.test_variant = variant;
        if (fastmem) {
#ifdef _WIN32
            arena = static_cast<u8*>(VirtualAlloc(nullptr, arena_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#endif
            conf.fastmem_pointer = arena;
            conf.fastmem_address_space_bits = 20;
            conf.silently_mirror_fastmem = false;
            env.arena = arena;
        }
        jit = std::make_unique<A64::Jit>(conf);
        env.jit = jit.get();
    }
    ~Runner() {
        jit.reset();
#ifdef _WIN32
        if (arena) {
            VirtualFree(arena, 0, MEM_RELEASE);
        }
#endif
    }
};

// Replay options, set by nxemu_jit_fuzz_replay (a script cannot reach this module's environment)
int g_only = -1, g_only_window = 0;
std::string g_keep;
std::string g_focus;  // comma-separated instruction names picked half of the time
int g_bench_reps = 0;  // > 0: also time each compiled sequence, run that many times, on both JITs

struct State {
    std::array<u64, 31> x;
    u64 sp;
    std::array<A64::Vector, 32> v;
    u32 fpcr, nzcv;
};

}  // namespace

extern "C"
#ifdef _WIN32
    __declspec(dllexport)
#endif
        void nxemu_jit_fuzz_replay(int only, int window, const char* keep) {
    g_only = only;
    g_only_window = window;
    g_keep = keep ? keep : "";
}

extern "C"
#ifdef _WIN32
    __declspec(dllexport)
#endif
        void nxemu_jit_fuzz_bench(int reps) {
    g_bench_reps = reps;
}

extern "C"
#ifdef _WIN32
    __declspec(dllexport)
#endif
        void nxemu_jit_fuzz_focus(const char* names) {
    g_focus = names ? names : "";
}

extern "C"
#ifdef _WIN32
    __declspec(dllexport)
#endif
        int nxemu_jit_fuzz(u64 seed, int iterations, u32 variant_a, u32 variant_b, int fastmem, const char* report_path) {
    FILE* rep = report_path && *report_path ? std::fopen(report_path, "w") : stdout;
    if (!rep) {
        return -1;
    }
    static const std::vector<Pattern> patterns = MakePatterns();
    std::vector<size_t> focus;
    for (size_t i = 0; i < patterns.size(); i++) {
        const std::string list = "," + g_focus + ",";
        if (!g_focus.empty() && list.find("," + std::string(patterns[i].fn) + ",") != std::string::npos) {
            focus.push_back(i);
        }
    }
    Runner a(variant_a, fastmem != 0), b(variant_b, fastmem != 0), probe(0, false);
    // An instruction alone, to drop encodings the translator rejects (reserved fields, unallocated)
    const auto valid = [&](u32 inst, const char* fn) {
        probe.env.code = {inst, svc0};
        probe.env.names = {fn};
        probe.env.bad = false;
        probe.env.done = false;
        probe.env.reasons = nullptr;
        probe.jit->InvalidateCacheRange(code_base, 4 * 32);
        probe.jit->SetPC(code_base);
        probe.jit->SetSP(0x80000);
        for (int i = 0; i < 16 && !probe.env.done; i++) {
            probe.jit->Run();
        }
        return !probe.env.bad;
    };
    int run = 0, skipped = 0, failed = 0;
    long long bench_a = 0, bench_b = 0;
    std::map<std::string, int> reasons;

    const int only = g_only;  // run one sequence alone
    const bool verbose_vectors = g_only >= 0;
    const int only_window = g_only_window;  // and the ones just before it
    for (int it = 0; it < iterations; it++) {
        if (only >= 0 && (it > only || it < only - only_window)) {
            continue;
        }
        std::mt19937_64 rng(seed * 0x9E3779B97F4A7C15ull + static_cast<u64>(it));  // one stream per sequence, to replay it alone
        // Random sequence of 1-24 instructions, then SVC #0
        const size_t n = 1 + rng() % 24;
        std::vector<u32> code;
        std::vector<const char*> names;
        while (code.size() < n) {
            const Pattern& p = !focus.empty() && rng() % 2 ? patterns[focus[rng() % focus.size()]] : patterns[rng() % patterns.size()];
            u32 inst = (static_cast<u32>(rng()) & ~p.mask) | p.expected;
            if (IsBranch(p.fn)) {
                inst = ForwardBranch(p.fn, inst, 1 + static_cast<u32>(rng() % (n - code.size())));
            }
            const auto m = A64::Decode<A64::TranslatorVisitor>(inst);
            if (!m || std::strcmp(m->get().GetName(), p.name) != 0 || !valid(inst, p.fn)) {
                continue;
            }
            code.push_back(inst);
            names.push_back(p.fn);
        }
        if (const char* keep = g_keep.empty() ? nullptr : g_keep.c_str(); keep && it == only) {
            // Replay with only some instructions (a 0/1 string), the others become NOP
            for (size_t i = 0; i < code.size(); i++) {
                if (i >= std::strlen(keep) || keep[i] != '1') {
                    code[i] = 0xD503201F;
                    names[i] = "NOP";
                }
            }
        }
        code.push_back(svc0);

        State s;
        for (auto& r : s.x) {
            // Mostly small values and addresses in the fastmem arena, sometimes any 64-bit value
            switch (rng() % 4) {
            case 0: r = rng(); break;
            case 1: r = rng() % 64; break;
            default: r = 0x20000 + (rng() % 0xC0000 & ~u64{7}); break;
            }
        }
        s.sp = 0x80000 + (rng() % 0x10000 & ~u64{15});
        // Half random, half special FP values (zeros, ones, NaNs, infinities) so that compares
        // also see equal and unordered operands
        static const u32 f32s[] = {0, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0x7FC00000, 0x7F800001, 0x7F800000, 0xFF800000};
        static const u64 f64s[] = {0, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x4000000000000000,
                                   0x7FF8000000000000, 0x7FF0000000000001, 0x7FF0000000000000, 0xFFF0000000000000};
        const auto f32 = [&] { return u64{f32s[rng() % (sizeof(f32s) / sizeof(f32s[0]))]}; };
        const auto f64 = [&] { return f64s[rng() % (sizeof(f64s) / sizeof(f64s[0]))]; };
        for (auto& v : s.v) {
            switch (rng() % 4) {
            case 2: v = {f32() | f32() << 32, f32() | f32() << 32}; break;
            case 3: v = {f64(), f64()}; break;
            default: v = {rng(), rng()}; break;
            }
        }
        static const u32 fpcrs[] = {0, 1u << 25, 1u << 24, 3u << 22, 1u << 22, (1u << 25) | (1u << 24)};
        s.fpcr = fpcrs[rng() % (sizeof(fpcrs) / sizeof(fpcrs[0]))];
        s.nzcv = static_cast<u32>(rng() % 16) << 28;

        for (Runner* r : {&a, &b}) {
            r->env.code = code;
            r->env.names = names;
            r->env.reasons = r == &a ? &reasons : nullptr;
            r->env.writes.clear();
            r->env.bad = false;
            r->env.done = false;
            if (r->arena) {
                static const std::vector<u8> initial = [] {
                    std::vector<u8> v(arena_size);
                    for (size_t i = 0; i < arena_size; i++) {
                        v[i] = InitialByte(i);
                    }
                    return v;
                }();
                std::memcpy(r->arena, initial.data(), arena_size);
            }
            r->tpidr = 0x1234;
            r->jit->InvalidateCacheRange(code_base, 4 * 32);
            r->jit->SetRegisters(s.x);
            r->jit->SetSP(s.sp);
            r->jit->SetVectors(s.v);
            r->jit->SetFpcr(s.fpcr);
            r->jit->SetFpsr(0);
            r->jit->SetPstate(s.nzcv);
            r->jit->SetPC(code_base);
            // A cache clear (cache full) halts the core at a block boundary; the emulator then runs again
            for (int i = 0; i < 16 && !r->env.done; i++) {
                r->jit->Run();
            }
        }
        if (a.env.bad || b.env.bad) {
            skipped++;
            continue;
        }
        run++;
        if (g_bench_reps > 0) {
            // The code is compiled now: time only execution. Both JITs run the same sequences
            // from the same state, interleaved, so machine noise hits both alike.
            for (int pass = 0; pass < 2; pass++) {
                Runner* r = (pass ^ (it & 1)) ? &b : &a;
                LARGE_INTEGER t0, t1;
                QueryPerformanceCounter(&t0);
                for (int k = 0; k < g_bench_reps; k++) {
                    r->env.done = false;
                    r->jit->SetRegisters(s.x);
                    r->jit->SetSP(s.sp);
                    r->jit->SetVectors(s.v);
                    r->jit->SetFpcr(s.fpcr);
                    r->jit->SetPstate(s.nzcv);
                    r->jit->SetPC(code_base);
                    r->jit->Run();
                }
                QueryPerformanceCounter(&t1);
                (r == &a ? bench_a : bench_b) += t1.QuadPart - t0.QuadPart;
            }
        }

        std::string diff;
        char buf[256];
        const auto xa = a.jit->GetRegisters(), xb = b.jit->GetRegisters();
        for (size_t i = 0; i < 31; i++) {
            if (xa[i] != xb[i]) {
                std::snprintf(buf, sizeof buf, " X%zu %016llx/%016llx", i, (unsigned long long)xa[i], (unsigned long long)xb[i]);
                diff += buf;
            }
        }
        const auto va = a.jit->GetVectors(), vb = b.jit->GetVectors();
        if (verbose_vectors) {
            for (size_t i = 0; i < 32; i++) {
                {
                    std::snprintf(buf, sizeof buf, " [Q%zu in %016llx%016llx out %016llx%016llx]", i, (unsigned long long)s.v[i][1], (unsigned long long)s.v[i][0],
                                  (unsigned long long)va[i][1], (unsigned long long)va[i][0]);
                    std::fprintf(rep, "%s", buf);
                }
            }
            std::fprintf(rep, "\n");
        }
        for (size_t i = 0; i < 32; i++) {
            if (va[i] != vb[i]) {
                std::snprintf(buf, sizeof buf, " Q%zu %016llx%016llx/%016llx%016llx", i, (unsigned long long)va[i][1], (unsigned long long)va[i][0],
                              (unsigned long long)vb[i][1], (unsigned long long)vb[i][0]);
                diff += buf;
            }
        }
        if (a.jit->GetSP() != b.jit->GetSP()) diff += " SP";
        if (a.jit->GetPC() != b.jit->GetPC()) diff += " PC";
        if ((a.jit->GetPstate() ^ b.jit->GetPstate()) & 0xF0000000) diff += " NZCV";
        if (a.jit->GetFpsr() != b.jit->GetFpsr()) {
            std::snprintf(buf, sizeof buf, " FPSR %08x/%08x", a.jit->GetFpsr(), b.jit->GetFpsr());
            diff += buf;
        }
        if (a.env.writes != b.env.writes) diff += " writes";
        if (a.arena && std::memcmp(a.arena, b.arena, arena_size) != 0) diff += " arena";
        if (diff.empty()) {
            continue;
        }
        failed++;
        if (failed <= 50) {
            std::fprintf(rep, "FAIL iteration %d fpcr %08x nzcv %x:%s\n  code:", it, s.fpcr, s.nzcv >> 28, diff.c_str());
            for (size_t i = 0; i < code.size(); i++) {
                std::fprintf(rep, " %08x%s%s", code[i], i < names.size() ? "/" : "", i < names.size() ? names[i] : "");
            }
            std::fprintf(rep, "\n");
            std::fflush(rep);
        }
    }
    if (g_bench_reps > 0 && run > 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        const double scale = 1e9 / static_cast<double>(f.QuadPart) / (static_cast<double>(run) * g_bench_reps);
        std::fprintf(rep, "bench: variant %u %.1f ns per run, variant %u %.1f ns per run (%+.2f %%)\n", variant_a, bench_a * scale, variant_b,
                     bench_b * scale, 100.0 * (static_cast<double>(bench_b) / static_cast<double>(bench_a) - 1.0));
    }
    std::fprintf(rep, "seed %llu variants %u/%u fastmem %d: %d run, %d skipped, %d failed\n", (unsigned long long)seed, variant_a, variant_b, fastmem, run,
                 skipped, failed);
    std::vector<std::pair<int, std::string>> top;
    for (const auto& [k, v] : reasons) {
        top.push_back({v, k});
    }
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 25; i++) {
        std::fprintf(rep, "skip %6d %s\n", top[i].first, top[i].second.c_str());
    }
    if (rep != stdout) {
        std::fclose(rep);
    }
    return failed;
}
