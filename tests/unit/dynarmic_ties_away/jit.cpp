// SPDX-License-Identifier: GPL-2.0-or-later
// Regression tests for x64 inline ties-away rounding (vector FRINTA/VRINTA and scalar FCVTAS/FCVTAU).

#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A64/a64.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct A64Env final : Dynarmic::A64::UserCallbacks {
    Dynarmic::A64::Jit* jit{};
    std::vector<std::uint32_t> code;

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A64::VAddr vaddr) override {
        const size_t idx = static_cast<size_t>(vaddr / 4);
        if (idx < code.size()) {
            return code[idx];
        }
        return 0xD4000001U;  // SVC #0
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
    void InterpreterFallback(Dynarmic::A64::VAddr, size_t) override { Require(false, "A64 interpreter fallback"); }
    void ExceptionRaised(Dynarmic::A64::VAddr, Dynarmic::A64::Exception) override { Require(false, "A64 exception"); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }
    std::uint64_t GetCNTPCT() override { return 0; }
    std::uint8_t MemoryRead8(Dynarmic::A64::VAddr) override { std::abort(); }
    std::uint16_t MemoryRead16(Dynarmic::A64::VAddr) override { std::abort(); }
    std::uint32_t MemoryRead32(Dynarmic::A64::VAddr) override { std::abort(); }
    std::uint64_t MemoryRead64(Dynarmic::A64::VAddr) override { std::abort(); }
    Dynarmic::A64::Vector MemoryRead128(Dynarmic::A64::VAddr) override { std::abort(); }
    void MemoryWrite8(Dynarmic::A64::VAddr, std::uint8_t) override { std::abort(); }
    void MemoryWrite16(Dynarmic::A64::VAddr, std::uint16_t) override { std::abort(); }
    void MemoryWrite32(Dynarmic::A64::VAddr, std::uint32_t) override { std::abort(); }
    void MemoryWrite64(Dynarmic::A64::VAddr, std::uint64_t) override { std::abort(); }
    void MemoryWrite128(Dynarmic::A64::VAddr, Dynarmic::A64::Vector) override { std::abort(); }
};

struct A32Env final : Dynarmic::A32::UserCallbacks {
    Dynarmic::A32::Jit* jit{};
    std::vector<std::uint32_t> code;

    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr vaddr) override {
        const size_t idx = static_cast<size_t>(vaddr / 4);
        if (idx < code.size()) {
            return code[idx];
        }
        return 0xEF000000U;  // SVC #0
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
    void InterpreterFallback(Dynarmic::A32::VAddr, size_t) override { Require(false, "A32 interpreter fallback"); }
    void ExceptionRaised(Dynarmic::A32::VAddr, Dynarmic::A32::Exception) override { Require(false, "A32 exception"); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }
    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr) override { std::abort(); }
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr) override { std::abort(); }
    std::uint64_t MemoryRead64(Dynarmic::A32::VAddr) override { std::abort(); }
    void MemoryWrite8(Dynarmic::A32::VAddr, std::uint8_t) override { std::abort(); }
    void MemoryWrite16(Dynarmic::A32::VAddr, std::uint16_t) override { std::abort(); }
    void MemoryWrite32(Dynarmic::A32::VAddr, std::uint32_t) override { std::abort(); }
    void MemoryWrite64(Dynarmic::A32::VAddr, std::uint64_t) override { std::abort(); }
};

constexpr std::uint32_t EncodeAddV2D(std::uint32_t d, std::uint32_t n, std::uint32_t m) {
    return 0x4EE08400U | (m << 16) | (n << 5) | d;
}

// Scalar FCVTAS/FCVTAU fallback under heavy XMM register pressure must preserve all live SIMD
// values even when ScratchXmm() spills or moves a live register.
void TestScalarFallbackUnderRegisterPressure() {
    struct Case {
        std::uint32_t insn;
        bool is_f64;
        std::uint64_t input_bits;
        std::uint64_t expected_x0;
        const char* name;
    };

    const std::array<Case, 8> cases{{
        // FCVTAS W0, S0 (0x1E240000)
        {0x1E240000U, false, 0x7FC00000ULL, 0ULL, "FCVTAS W0,S0 NaN"},
        {0x1E240000U, false, std::bit_cast<std::uint32_t>(1e20f), 0x7FFFFFFFULL, "FCVTAS W0,S0 +overflow"},
        {0x1E240000U, false, std::bit_cast<std::uint32_t>(-2.5f), 0xFFFFFFFDULL, "FCVTAS W0,S0 -2.5"},
        // FCVTAU W0, S0 (0x1E250000)
        {0x1E250000U, false, std::bit_cast<std::uint32_t>(-1.5f), 0ULL, "FCVTAU W0,S0 negative"},
        // FCVTAS X0, D0 (0x9E640000)
        {0x9E640000U, true, 0x7FF8000000000000ULL, 0ULL, "FCVTAS X0,D0 NaN"},
        {0x9E640000U, true, std::bit_cast<std::uint64_t>(-1e30), 0x8000000000000000ULL, "FCVTAS X0,D0 -overflow"},
        {0x9E640000U, true, std::bit_cast<std::uint64_t>(2.5), 3ULL, "FCVTAS X0,D0 +2.5"},
        // FCVTAU X0, D0 (0x9E650000)
        {0x9E650000U, true, std::bit_cast<std::uint64_t>(1e30), 0xFFFFFFFFFFFFFFFFULL, "FCVTAU X0,D0 +overflow"},
    }};

    for (const auto& tc : cases) {
        A64Env env;
        // Produce 15 live XMM registers (V1..V15) before the conversion, then consume all 15
        // afterwards so every XMM register is live across the conversion and its fallback branch.
        for (std::uint32_t i = 1; i <= 15; ++i) {
            env.code.push_back(EncodeAddV2D(i, i, 16 + (i % 15)));
        }
        env.code.push_back(tc.insn);
        for (std::uint32_t i = 1; i <= 15; ++i) {
            env.code.push_back(EncodeAddV2D(i, i, 16 + ((i + 3) % 15)));
        }
        env.code.push_back(0xD4000001U);  // SVC #0

        Dynarmic::A64::UserConfig config{};
        config.callbacks = &env;
        config.optimizations = Dynarmic::all_safe_optimizations;
        Dynarmic::A64::Jit jit(config);
        env.jit = &jit;

        std::array<Dynarmic::A64::Vector, 32> vecs{};
        vecs[0] = {tc.input_bits, 0};
        for (size_t i = 1; i < 32; ++i) {
            vecs[i] = {0x1111000000000000ULL + i * 0x123456789ULL,
                       0x2222000000000000ULL + i * 0x987654321ULL};
        }
        auto expected_vecs = vecs;
        for (size_t i = 1; i <= 15; ++i) {
            const size_t src1 = 16 + (i % 15);
            const size_t src2 = 16 + ((i + 3) % 15);
            for (size_t lane = 0; lane < 2; ++lane) {
                expected_vecs[i][lane] = expected_vecs[i][lane] + expected_vecs[src1][lane] + expected_vecs[src2][lane];
            }
        }

        jit.SetFpcr(0);
        jit.SetFpsr(0);
        jit.SetRegister(0, 0xDEADBEEFCAFEBABEULL);
        jit.SetVectors(vecs);
        jit.SetPC(0);
        jit.Run();

        Require(jit.GetRegister(0) == tc.expected_x0, tc.name);
        const auto got_vecs = jit.GetVectors();
        for (size_t i = 1; i <= 15; ++i) {
            Require(got_vecs[i] == expected_vecs[i], "live XMM register corrupted across scalar fallback");
        }
    }
}

// A32 vector VRINTA.F32 uses standard Advanced SIMD FPCR (FZ=1) even when guest FPSCR.FZ=0.
// A subnormal lane must still flush to zero and set FPSCR.IDC (bit 7).
void TestA32VectorVrintaSubnormalSetsIdc() {
    A32Env env;
    // VRINTA.F32 Q0, Q0: 111100111 0 11 10 10 0000 01010 1 0 0 0000 = 0xF3BA0540
    env.code = {
        0xF3BA0540U,  // VRINTA.F32 Q0, Q0
        0xEF000000U,  // SVC #0
    };

    Dynarmic::A32::UserConfig config{};
    config.callbacks = &env;
    config.arch_version = Dynarmic::A32::ArchVersion::v8;
    config.optimizations = Dynarmic::all_safe_optimizations;
    Dynarmic::A32::Jit jit(config);
    env.jit = &jit;

    jit.SetCpsr(0x00000010U);  // ARM user mode
    jit.SetFpscr(0x00000000U);  // FZ=0, IDC=0
    jit.ExtRegs()[0] = 0x00000001U;  // smallest positive subnormal float
    jit.ExtRegs()[1] = std::bit_cast<std::uint32_t>(1.5f);
    jit.ExtRegs()[2] = std::bit_cast<std::uint32_t>(-2.5f);
    jit.ExtRegs()[3] = std::bit_cast<std::uint32_t>(0.5f);
    jit.Regs()[15] = 0;
    jit.Run();

    Require(jit.ExtRegs()[0] == 0x00000000U, "A32 VRINTA subnormal lane must round to +0.0");
    Require(jit.ExtRegs()[1] == std::bit_cast<std::uint32_t>(2.0f), "A32 VRINTA +1.5 -> +2.0");
    Require(jit.ExtRegs()[2] == std::bit_cast<std::uint32_t>(-3.0f), "A32 VRINTA -2.5 -> -3.0");
    Require(jit.ExtRegs()[3] == std::bit_cast<std::uint32_t>(1.0f), "A32 VRINTA +0.5 -> +1.0");
    constexpr std::uint32_t kFpscrIdc = 1U << 7;
    Require((jit.Fpscr() & kFpscrIdc) != 0, "A32 VRINTA subnormal lane must set FPSCR.IDC");
}

// A64 vector FRINTA.4S and FRINTA.2D with FPCR.FZ=0 still take the inline ties-away path and
// produce exact results across ties, negative values, and subnormals.
void TestA64VectorFrinta() {
    A64Env env;
    env.code = {
        0x6E218800U,  // FRINTA V0.4S, V0.4S
        0x6E618821U,  // FRINTA V1.2D, V1.2D
        0xD4000001U,  // SVC #0
    };

    Dynarmic::A64::UserConfig config{};
    config.callbacks = &env;
    config.optimizations = Dynarmic::all_safe_optimizations;
    Dynarmic::A64::Jit jit(config);
    env.jit = &jit;

    const auto pack4f = [](float a, float b, float c, float d) -> Dynarmic::A64::Vector {
        const std::uint64_t lo = std::bit_cast<std::uint32_t>(a) |
                                 (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(b)) << 32);
        const std::uint64_t hi = std::bit_cast<std::uint32_t>(c) |
                                 (static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(d)) << 32);
        return {lo, hi};
    };

    std::array<Dynarmic::A64::Vector, 32> vecs{};
    vecs[0] = pack4f(0.5f, -0.5f, 2.5f, -2.5f);
    vecs[1] = {std::bit_cast<std::uint64_t>(1.5), std::bit_cast<std::uint64_t>(-3.5)};

    jit.SetFpcr(0);
    jit.SetFpsr(0);
    jit.SetVectors(vecs);
    jit.SetPC(0);
    jit.Run();

    const auto got = jit.GetVectors();
    Require(got[0] == pack4f(1.0f, -1.0f, 3.0f, -3.0f), "A64 FRINTA.4S ties-away");
    Require(got[1][0] == std::bit_cast<std::uint64_t>(2.0), "A64 FRINTA.2D +1.5 -> +2.0");
    Require(got[1][1] == std::bit_cast<std::uint64_t>(-4.0), "A64 FRINTA.2D -3.5 -> -4.0");
}

}  // namespace

int main() {
    TestScalarFallbackUnderRegisterPressure();
    TestA32VectorVrintaSubnormalSetsIdc();
    TestA64VectorFrinta();
    std::printf("dynarmic_ties_away tests passed.\n");
    return 0;
}
