// Guest state helpers shared by the step 3 and step 4 tests.
#pragma once

#include <cstdint>
#include <cstring>
#include <random>
#include <string>

#include "nce_runtime.h"
#include "report.h"

namespace ncev {

constexpr std::uint32_t NzcvMask = 0xF0000000u;

struct GuestState {
    std::uint64_t x[31];
    std::uint64_t sp;
    std::uint64_t pc;
    std::uint32_t nzcv;
    std::uint32_t fpcr;
    std::uint32_t fpsr;
    std::uint64_t v[32][2];
    std::uint64_t tpidr_el0;
    std::uint64_t tpidrro_el0;
};

inline void LoadState(Core::GuestContext & c, const GuestState & s) {
    for (int i = 0; i < 31; i++) {
        c.cpu_registers[i] = s.x[i];
    }
    c.sp = s.sp;
    c.pc = s.pc;
    // ArmNce::SetContext writes pstate; the SVC exit writes nzcv. Both start equal.
    c.pstate = s.nzcv;
    c.fpcr = s.fpcr;
    c.fpsr = s.fpsr;
    std::memcpy(c.vector_registers.data(), s.v, sizeof(s.v));
    c.tpidr_el0 = s.tpidr_el0;
    c.tpidrro_el0 = s.tpidrro_el0;
}

// nzcv_from_pstate: signal exits store the flags in pstate, SVC exits in nzcv.
inline GuestState StoreState(const Core::GuestContext & c, bool nzcv_from_pstate) {
    GuestState s{};
    for (int i = 0; i < 31; i++) {
        s.x[i] = c.cpu_registers[i];
    }
    s.sp = c.sp;
    s.pc = c.pc;
    s.nzcv = c.pstate & NzcvMask;
    s.fpcr = c.fpcr;
    s.fpsr = c.fpsr;
    std::memcpy(s.v, c.vector_registers.data(), sizeof(s.v));
    s.tpidr_el0 = c.tpidr_el0;
    s.tpidrro_el0 = c.tpidrro_el0;
    return s;
}

inline GuestState RandomState(std::mt19937_64 & rng, std::uint64_t sp, const HostFpMasks & fp) {
    GuestState s{};
    for (auto & r : s.x) {
        r = rng();
    }
    s.sp = sp;
    s.nzcv = static_cast<std::uint32_t>(rng() & 0xF) << 28;
    // AHP, DN, FZ and RMode only: no floating-point trap enables.
    s.fpcr = static_cast<std::uint32_t>(rng()) & fp.fpcr_writable & 0x07C00000u;
    s.fpsr = static_cast<std::uint32_t>(rng()) & fp.fpsr_writable;
    for (auto & q : s.v) {
        q[0] = rng();
        q[1] = rng();
    }
    s.tpidr_el0 = rng();
    s.tpidrro_el0 = rng();
    return s;
}

struct DiffResult {
    int mismatches = 0;
    bool x18_lost = false; // also counted in mismatches
    std::string text;
};

inline DiffResult Diff(const GuestState & expected, const GuestState & got) {
    DiffResult d;
    auto add = [&](const std::string & what) {
        d.mismatches++;
        if (d.text.size() < 300) {
            d.text += what + " ";
        }
    };
    for (int i = 0; i < 31; i++) {
        if (expected.x[i] != got.x[i]) {
            d.x18_lost |= i == 18;
            add(Fmt("x%d=%#llx(expected %#llx)", i, (unsigned long long)got.x[i], (unsigned long long)expected.x[i]));
        }
    }
    if (expected.sp != got.sp) {
        add(Fmt("sp=%#llx(expected %#llx)", (unsigned long long)got.sp, (unsigned long long)expected.sp));
    }
    if (expected.pc != got.pc) {
        add(Fmt("pc=%#llx(expected %#llx)", (unsigned long long)got.pc, (unsigned long long)expected.pc));
    }
    if (expected.nzcv != got.nzcv) {
        add(Fmt("nzcv=%#x(expected %#x)", got.nzcv, expected.nzcv));
    }
    if (expected.fpcr != got.fpcr) {
        add(Fmt("fpcr=%#x(expected %#x)", got.fpcr, expected.fpcr));
    }
    if (expected.fpsr != got.fpsr) {
        add(Fmt("fpsr=%#x(expected %#x)", got.fpsr, expected.fpsr));
    }
    for (int i = 0; i < 32; i++) {
        if (expected.v[i][0] != got.v[i][0] || expected.v[i][1] != got.v[i][1]) {
            add(Fmt("v%d", i));
        }
    }
    if (expected.tpidr_el0 != got.tpidr_el0) {
        add("tpidr_el0");
    }
    return d;
}

} // namespace ncev
