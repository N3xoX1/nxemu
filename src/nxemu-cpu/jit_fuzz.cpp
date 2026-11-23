// Differential test of the A64 JIT: random instruction sequences run on two JITs whose configs
// differ only by UserConfig::test_variant; registers, flags, FPSR and memory writes must match.
// Called from a script through the exported nxemu_jit_fuzz (no emulator needed). nxemu_jit_stress
// runs cores that share a code cache while another thread invalidates their code.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
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
constexpr uint64_t arena_size = 1 << 20;  // fastmem mode: guest addresses below 1 MiB
constexpr uint64_t arena_pages = arena_size >> 12;
constexpr uint64_t write_track_l1_offset = arena_pages + 64;
constexpr uint64_t write_track_size = arena_pages + 3 * 64;

constexpr int fastmem_unchecked = 2;
constexpr uint64_t unchecked_reservation = 2 * arena_size;

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
    std::vector<bool> callback_pages;  // arena pages written by the callbacks, which the JIT does not mark
    A64::Jit* jit = nullptr;
    bool bad = false;  // exception or interpreter fallback: the sequence is skipped
    bool done = false;  // reached the final SVC or was skipped
    bool detect_out_of_reservation = false;
    bool out_of_reservation = false;
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
        if (detect_out_of_reservation && a >= unchecked_reservation) {
            out_of_reservation = true;
        }
        if (arena && a < arena_size) {
            return arena[a];
        }
        const auto it = writes.find(a);
        return it != writes.end() ? it->second : InitialByte(a);
    }
    void Write(u64 a, u8 v) {
        if (detect_out_of_reservation && a >= unchecked_reservation) {
            out_of_reservation = true;
        }
        if (arena && a < arena_size) {
            arena[a] = v;
            callback_pages[a >> 12] = true;
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
    u8* write_track = nullptr;
    bool write_track_virtual = false;

    // A low write tracking map is addressed without a base register
    Runner(u32 variant, int fastmem, bool low_write_track = false) {
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
            if (fastmem == fastmem_unchecked) {
                arena = static_cast<u8*>(VirtualAlloc(nullptr, unchecked_reservation, MEM_RESERVE, PAGE_NOACCESS));
                VirtualAlloc(arena, arena_size, MEM_COMMIT, PAGE_READWRITE);
            } else {
                arena = static_cast<u8*>(VirtualAlloc(nullptr, arena_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            }
#endif
            conf.fastmem_pointer = arena;
            conf.fastmem_address_space_bits = fastmem == fastmem_unchecked ? 64 : 20;
            conf.silently_mirror_fastmem = false;
            if (fastmem == fastmem_unchecked) {
                conf.recompile_on_fastmem_failure = false;
            }
            env.arena = arena;
            env.callback_pages.resize(arena_pages);
#ifdef _WIN32
            for (u64 address = 0x40000000; low_write_track && !write_track && address < 0x78000000; address += 0x1000000) {
                write_track = static_cast<u8*>(VirtualAlloc(reinterpret_cast<void*>(address), write_track_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            }
            write_track_virtual = write_track != nullptr;
#endif
            if (!write_track) {
                write_track = new u8[write_track_size]();
            }
            conf.write_track_map = write_track;
            conf.write_track_l2_offset = static_cast<u32>(arena_pages);
            conf.write_track_l1_offset = static_cast<u32>(write_track_l1_offset);
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
        if (write_track_virtual) {
            VirtualFree(write_track, 0, MEM_RELEASE);
            write_track = nullptr;
        }
#endif
        delete[] write_track;
    }

    // Every arena page that changed must be marked, with its summary bytes
    bool WriteMarksMissing(const u8* initial) const {
        for (uint64_t page = 0; write_track && page < arena_pages; page++) {
            const bool marked = write_track[page] && write_track[arena_pages + (page >> 9)] && write_track[write_track_l1_offset + (page >> 18)];
            if (!marked && !env.callback_pages[page] && std::memcmp(arena + page * 4096, initial + page * 4096, 4096) != 0) {
                return true;
            }
        }
        return false;
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
    if (g_bench_reps > 0 && fastmem == fastmem_unchecked) {
        std::fprintf(rep, "bench does not run with fastmem 2: the arena check only covers the first run\n");
        if (rep != stdout) {
            std::fclose(rep);
        }
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
    Runner a(variant_a, fastmem, true), b(variant_b, fastmem), probe(0, 0);
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

    static const std::vector<u8> initial_arena = [] {
        std::vector<u8> v(arena_size);
        for (uint64_t i = 0; i < arena_size; i++) {
            v[i] = InitialByte(i);
        }
        return v;
    }();
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
            default:
                r = 0x20000 + (rng() % 0xC0000 & ~u64{7});
                if (fastmem == fastmem_unchecked && rng() % 3 == 0) {
                    r += arena_size;
                }
                break;
            }
        }
        s.sp = 0x80000 + (rng() % 0x10000 & ~u64{15});
        // Half random, half special FP values so that compares also see equal and unordered
        // operands, and roundings see ties and the edges of the values with a fraction
        static const u32 f32s[] = {0, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0x7FC00000, 0x7F800001, 0x7F800000, 0xFF800000,
                                   0x3F000000, 0x3FC00000, 0xC0200000, 0xBEFFFFFF, 0x4AFFFFFF, 0x4B000000};
        static const u64 f64s[] = {0, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x4000000000000000,
                                   0x7FF8000000000000, 0x7FF0000000000001, 0x7FF0000000000000, 0xFFF0000000000000,
                                   0x3FE0000000000000, 0x3FF8000000000000, 0xC004000000000000, 0xBFDFFFFFFFFFFFFF,
                                   0x432FFFFFFFFFFFFF, 0x4330000000000000};
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

        if (fastmem == fastmem_unchecked) {
            // Unchecked, a JIT access past the reservation would reach host memory
            probe.env.code = code;
            probe.env.names = names;
            probe.env.writes.clear();
            probe.env.bad = false;
            probe.env.done = false;
            probe.env.reasons = nullptr;
            probe.env.detect_out_of_reservation = true;
            probe.env.out_of_reservation = false;
            probe.tpidr = 0x1234;
            probe.jit->InvalidateCacheRange(code_base, 4 * 32);
            probe.jit->SetRegisters(s.x);
            probe.jit->SetSP(s.sp);
            probe.jit->SetVectors(s.v);
            probe.jit->SetFpcr(s.fpcr);
            probe.jit->SetFpsr(0);
            probe.jit->SetPstate(s.nzcv);
            probe.jit->SetPC(code_base);
            for (int i = 0; i < 16 && !probe.env.done && !probe.env.out_of_reservation; i++) {
                probe.jit->Run();
            }
            probe.env.detect_out_of_reservation = false;
            if (probe.env.out_of_reservation || probe.env.bad || !probe.env.done) {
                skipped++;
                reasons[probe.env.out_of_reservation ? "leaves the fastmem arena" : "the arena check did not finish"]++;
                continue;
            }
        }

        for (Runner* r : {&a, &b}) {
            r->env.code = code;
            r->env.names = names;
            r->env.reasons = r == &a ? &reasons : nullptr;
            r->env.writes.clear();
            r->env.bad = false;
            r->env.done = false;
            if (r->arena) {
                std::memcpy(r->arena, initial_arena.data(), arena_size);
                std::memset(r->write_track, 0, write_track_size);
                r->env.callback_pages.assign(arena_pages, false);
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
                const auto t0 = std::chrono::steady_clock::now();
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
                const auto t1 = std::chrono::steady_clock::now();
                (r == &a ? bench_a : bench_b) += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
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
        if (a.arena && a.WriteMarksMissing(initial_arena.data())) diff += " marks(a)";
        if (b.arena && b.WriteMarksMissing(initial_arena.data())) diff += " marks(b)";
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
        const double scale = 1.0 / (static_cast<double>(run) * g_bench_reps);
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

namespace {

using s64 = std::int64_t;

u32 EncAddImm(u32 rd, u32 rn, u32 imm) { return 0x91000000u | (imm & 0xFFF) << 10 | rn << 5 | rd; }
u32 EncSubImm(u32 rd, u32 rn, u32 imm) { return 0xD1000000u | (imm & 0xFFF) << 10 | rn << 5 | rd; }
u32 EncSubsImm(u32 rd, u32 rn, u32 imm) { return 0xF1000000u | (imm & 0xFFF) << 10 | rn << 5 | rd; }
u32 EncAddShift(u32 rd, u32 rn, u32 rm, u32 shift, u32 amount) { return 0x8B000000u | shift << 22 | rm << 16 | amount << 10 | rn << 5 | rd; }
u32 EncEorShift(u32 rd, u32 rn, u32 rm, u32 shift, u32 amount) { return 0xCA000000u | shift << 22 | rm << 16 | amount << 10 | rn << 5 | rd; }
u32 EncMadd(u32 rd, u32 rn, u32 rm, u32 ra) { return 0x9B000000u | rm << 16 | ra << 10 | rn << 5 | rd; }
u32 EncMovz(u32 rd, u32 imm, u32 hw) { return 0xD2800000u | hw << 21 | (imm & 0xFFFF) << 5 | rd; }
u32 EncCbz(u32 rt, bool nz) { return (nz ? 0xB5000000u : 0xB4000000u) | rt; }
u32 EncTbz(u32 rt, u32 bit, bool nz) { return (nz ? 0x37000000u : 0x36000000u) | (bit >> 5) << 31 | (bit & 31) << 19 | rt; }
u32 EncBcond(u32 cond) { return 0x54000000u | cond; }
u32 EncBl(s64 offset) { return 0x94000000u | (static_cast<u32>(offset / 4) & 0x3FFFFFF); }
u32 EncAdr(u32 rd, s64 offset) {
    const u32 imm = static_cast<u32>(offset) & 0x1FFFFF;
    return 0x10000000u | (imm & 3) << 29 | (imm >> 2) << 5 | rd;
}
u32 EncBlr(u32 rn) { return 0xD63F0000u | rn << 5; }
constexpr u32 ret_inst = 0xD65F03C0u;
constexpr u32 cond_ne = 1;

enum class StressOpKind { Plain, Call, Adr, Branch };

struct StressOp {
    StressOpKind kind;
    u32 inst;    // Branch: CBZ, CBNZ, TBZ, TBNZ or B.cond with a zero offset
    s64 target;  // Call, Adr: function; Branch: instructions forward
};

// main calls the functions X19 times; a function may call a later one and keeps its LR in
// X(19 + function), so the calls nest without a stack
struct StressProgram {
    std::vector<u32> code;
    std::vector<std::pair<u64, u64>> functions;  // start, size in bytes
};

StressProgram MakeStressProgram(std::mt19937_64& rng) {
    const auto rnd = [&](u32 n) { return static_cast<u32>(rng() % n); };
    const u32 function_count = 4 + rnd(5);
    std::vector<std::vector<StressOp>> bodies(function_count + 1);
    std::vector<size_t> limits(function_count + 1);  // last instruction a branch may reach

    std::vector<StressOp>& main_body = bodies[0];
    for (u32 i = 0, calls = 3 + rnd(6); i < calls; i++) {
        const s64 function = 1 + rnd(function_count);
        switch (rnd(3)) {
        case 0:
            main_body.push_back({StressOpKind::Call, 0, function});
            break;
        case 1:
            main_body.push_back({StressOpKind::Adr, 0, function});
            main_body.push_back({StressOpKind::Plain, EncBlr(13), 0});
            break;
        case 2:
            main_body.push_back({StressOpKind::Branch, EncTbz(rnd(8), rnd(64), rnd(2) != 0), 2});
            main_body.push_back({StressOpKind::Call, 0, function});
            break;
        }
    }
    limits[0] = main_body.size();
    main_body.push_back({StressOpKind::Plain, EncSubsImm(19, 19, 1), 0});
    main_body.push_back({StressOpKind::Plain, EncBcond(cond_ne) | (static_cast<u32>(-static_cast<s64>(main_body.size())) & 0x7FFFF) << 5, 0});
    main_body.push_back({StressOpKind::Plain, svc0, 0});

    for (u32 k = 1; k <= function_count; k++) {
        std::vector<StressOp>& body = bodies[k];
        const u32 lr_save = 19 + k;
        const bool calls = k < function_count && rnd(2) != 0;
        if (calls) {
            body.push_back({StressOpKind::Plain, EncAddImm(lr_save, 30, 0), 0});
        }
        const u32 count = 3 + rnd(8);
        const u32 call_at = calls ? rnd(count) : count;
        for (u32 i = 0; i < count; i++) {
            if (i == call_at) {
                body.push_back({StressOpKind::Call, 0, k + 1 + rnd(function_count - k)});
            }
            const u32 rd = rnd(8), rn = rnd(8), rm = rnd(8);
            const s64 skip = 2 + rnd(3);
            switch (rnd(9)) {
            case 0: body.push_back({StressOpKind::Plain, EncAddImm(rd, rn, rnd(4096)), 0}); break;
            case 1: body.push_back({StressOpKind::Plain, EncSubImm(rd, rn, rnd(4096)), 0}); break;
            case 2: body.push_back({StressOpKind::Plain, EncAddShift(rd, rn, rm, rnd(2), rnd(64)), 0}); break;
            case 3: body.push_back({StressOpKind::Plain, EncEorShift(rd, rn, rm, rnd(2), rnd(64)), 0}); break;
            case 4: body.push_back({StressOpKind::Plain, EncMadd(rd, rn, rm, rnd(8)), 0}); break;
            case 5: body.push_back({StressOpKind::Plain, EncMovz(rd, rnd(65536), rnd(4)), 0}); break;
            case 6: body.push_back({StressOpKind::Branch, EncCbz(rd, rnd(2) != 0), skip}); break;
            case 7: body.push_back({StressOpKind::Branch, EncTbz(rd, rnd(64), rnd(2) != 0), skip}); break;
            case 8:
                body.push_back({StressOpKind::Plain, EncSubsImm(8, rn, rnd(4096)), 0});
                body.push_back({StressOpKind::Branch, EncBcond(rnd(14)), skip});
                break;
            }
        }
        limits[k] = body.size();
        if (calls) {
            body.push_back({StressOpKind::Plain, EncAddImm(30, lr_save, 0), 0});
        }
        body.push_back({StressOpKind::Plain, ret_inst, 0});
    }

    std::vector<u64> starts(function_count + 1);
    u64 address = code_base;
    for (size_t k = 0; k < bodies.size(); k++) {
        starts[k] = address;
        address += 4 * bodies[k].size();
    }
    StressProgram program;
    for (size_t k = 0; k < bodies.size(); k++) {
        for (size_t i = 0; i < bodies[k].size(); i++) {
            const StressOp& op = bodies[k][i];
            const s64 here = static_cast<s64>(starts[k] + 4 * i);
            u32 inst = op.inst;
            if (op.kind == StressOpKind::Call) {
                inst = EncBl(static_cast<s64>(starts[op.target]) - here);
            } else if (op.kind == StressOpKind::Adr) {
                inst = EncAdr(13, static_cast<s64>(starts[op.target]) - here);
            } else if (op.kind == StressOpKind::Branch) {
                const u32 offset = static_cast<u32>(std::min<s64>(op.target, static_cast<s64>(limits[k] - i)));
                const bool test_bit = (inst & 0x7E000000u) == 0x36000000u;
                inst |= (offset & (test_bit ? 0x3FFFu : 0x7FFFFu)) << 5;
            }
            program.code.push_back(inst);
        }
        if (k > 0) {
            program.functions.push_back({starts[k], 4 * bodies[k].size()});
        }
    }
    return program;
}

struct StressCore {
    Env env;
    u64 tpidr = 0, tpidrro = 0;
    std::unique_ptr<A64::Jit> jit;

    StressCore(const StressProgram& program, const void* share_key, u32 variant, bool cycle_counting, size_t processor_id) {
        env.code = program.code;
        A64::UserConfig conf;
        conf.callbacks = &env;
        conf.tpidr_el0 = &tpidr;
        conf.tpidrro_el0 = &tpidrro;
        conf.define_unpredictable_behaviour = true;
        conf.enable_cycle_counting = cycle_counting;
        conf.code_cache_size = 32 * 1024 * 1024;
        conf.test_variant = variant;
        conf.share_code_cache_key = share_key;
        conf.processor_id = processor_id;
        jit = std::make_unique<A64::Jit>(conf);
        env.jit = jit.get();
    }
    void Start(u64 seed, u64 loops) {
        for (size_t r = 0; r < 31; r++) {
            jit->SetRegister(r, (seed + r) * 0x9E3779B97F4A7C15ull);
        }
        jit->SetRegister(19, loops);
        jit->SetSP(0x80000);
        jit->SetPC(code_base);
    }
    void RunToEnd() {
        while (!env.done) {
            jit->Run();
        }
    }
    std::string Result() const {
        std::string result;
        for (size_t r = 0; r < 31; r++) {
            result += std::to_string(jit->GetRegister(r)) + ",";
        }
        return result + std::to_string(jit->GetPC()) + "," + std::to_string(jit->GetPstate() >> 28) + (env.bad ? ",bad" : "");
    }
};

}  // namespace

// Cores sharing one code cache run the same program while this thread invalidates its functions,
// clears the cache and halts cores; each core must end like a JIT of its own left alone.
extern "C"
#ifdef _WIN32
    __declspec(dllexport)
#endif
        int nxemu_jit_stress(u64 seed, int rounds, int core_count, u32 variant, int clear_permille, const char* report_path) {
    FILE* rep = report_path && *report_path ? std::fopen(report_path, "w") : stdout;
    if (!rep) {
        return -1;
    }
    static const int share_key = 0;
    int failed = 0;
    u64 invalidations = 0, clears = 0;
    for (int round = 0; round < rounds; round++) {
        std::mt19937_64 rng(seed * 0x9E3779B97F4A7C15ull + static_cast<u64>(round));
        const StressProgram program = MakeStressProgram(rng);
        const u64 loops = 50000 + rng() % 200000;
        const bool cycle_counting = (round & 1) != 0;

        std::vector<std::unique_ptr<StressCore>> cores;
        for (int c = 0; c < core_count; c++) {
            cores.push_back(std::make_unique<StressCore>(program, &share_key, variant, cycle_counting, c));
            cores.back()->Start(seed + round * 16 + c, loops);
        }
        std::atomic<int> running{core_count};
        std::vector<std::thread> threads;
        for (int c = 0; c < core_count; c++) {
            threads.emplace_back([&running, &core = *cores[c]] {
                core.RunToEnd();
                running--;
            });
        }
        while (running.load() > 0) {
            static const u64 spin_limits[] = {1000, 20000, 300000};
            for (volatile u64 i = 0, spin = rng() % spin_limits[rng() % 3]; i < spin; i = i + 1) {
            }
            StressCore& target = *cores[rng() % core_count];
            if (static_cast<int>(rng() % 1000) < clear_permille) {
                target.jit->ClearCache();
                clears++;
            } else {
                const std::pair<u64, u64>& function = program.functions[rng() % program.functions.size()];
                target.jit->InvalidateCacheRange(function.first, function.second);
                invalidations++;
            }
            if (rng() % 4 == 0) {
                target.jit->HaltExecution(HaltReason::UserDefined1);
            }
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        for (int c = 0; c < core_count; c++) {
            StressCore reference(program, nullptr, variant, cycle_counting, 0);
            reference.Start(seed + round * 16 + c, loops);
            reference.RunToEnd();
            const std::string want = reference.Result(), got = cores[c]->Result();
            if (want == got) {
                continue;
            }
            failed++;
            if (failed <= 20) {
                std::fprintf(rep, "FAIL round %d core %d\n  want %s\n  got  %s\n", round, c, want.c_str(), got.c_str());
                std::fflush(rep);
            }
        }
    }
    std::fprintf(rep, "stress seed %llu rounds %d cores %d variant %u clear %d: %d failed, %llu invalidations, %llu clears\n", (unsigned long long)seed,
                 rounds, core_count, variant, clear_permille, failed, (unsigned long long)invalidations, (unsigned long long)clears);
    if (rep != stdout) {
        std::fclose(rep);
    }
    return failed;
}
