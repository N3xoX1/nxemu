// SPDX-License-Identifier: GPL-2.0-or-later
// Compile and execute real AArch64 blocks with every GPR and SIMD register live.
#include "dynarmic/interface/A64/a64.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {
using namespace Dynarmic;
using namespace Dynarmic::A64;
using Clock = std::chrono::steady_clock;
void Require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
struct Environment final : UserCallbacks {
    Jit *jit{};
    size_t rounds{1}, compilations{}, svc_calls{};
    Clock::time_point start;
    std::chrono::nanoseconds compilation_time{};
    std::optional<std::uint32_t> MemoryReadCode(VAddr address) override {
        const auto index = (address & 4095) / 4;
        const auto operation = index % 63;
        const auto salt = (address >> 12) % 29;
        if (index == rounds * 63)
            return 0xeb01001f; // CMP X0, X1
        if (index == rounds * 63 + 1)
            return 0x9a820020; // CSEL X0, X1, X2, EQ
        if (index > rounds * 63 + 1)
            return 0xd4000001; // SVC #0
        if (operation < 31) {
            const auto d = operation;
            const auto n = (d + 1 + salt) % 31;
            const auto m = (d + 7) % 31;
            return static_cast<std::uint32_t>(0x8b000000 | (m << 16) | (n << 5) | d);
        }
        const auto d = operation - 31;
        const auto n = (d + 1 + salt) % 32;
        const auto m = (d + 7) % 32;
        return static_cast<std::uint32_t>(0x4ee08400 | (m << 16) | (n << 5) | d);
    }
    void OnCompilation(bool begin) override {
        if (begin) {
            start = Clock::now();
            ++compilations;
        } else
            compilation_time += Clock::now() - start;
    }
    void CallSVC(std::uint32_t) override {
        ++svc_calls;
        jit->HaltExecution();
    }
    void InterpreterFallback(VAddr, size_t) override { Require(false, "interpreter fallback"); }
    void ExceptionRaised(VAddr, Exception) override { Require(false, "unexpected exception"); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1000000; }
    std::uint64_t GetCNTPCT() override { return 0; }
    std::uint8_t MemoryRead8(VAddr) override { std::abort(); }
    std::uint16_t MemoryRead16(VAddr) override { std::abort(); }
    std::uint32_t MemoryRead32(VAddr) override { std::abort(); }
    std::uint64_t MemoryRead64(VAddr) override { std::abort(); }
    Vector MemoryRead128(VAddr) override { std::abort(); }
    void MemoryWrite8(VAddr, std::uint8_t) override { std::abort(); }
    void MemoryWrite16(VAddr, std::uint16_t) override { std::abort(); }
    void MemoryWrite32(VAddr, std::uint32_t) override { std::abort(); }
    void MemoryWrite64(VAddr, std::uint64_t) override { std::abort(); }
    void MemoryWrite128(VAddr, Vector) override { std::abort(); }
};

void Run(size_t blocks, size_t rounds, OptimizationFlag optimizations) {
    Environment env;
    env.rounds = rounds;
    UserConfig config{};
    config.callbacks = &env;
    config.optimizations = optimizations;
    config.notify_compilation = true;
    config.code_cache_size = 256 * 1024 * 1024;
    Jit jit(config);
    env.jit = &jit;
    auto pass = [&](bool warm) {
        const auto begin = Clock::now();
        for (size_t block = 0; block < blocks; ++block) {
            std::array<std::uint64_t, 31> expected;
            std::array<Vector, 32> vectors;
            for (size_t i = 0; i < expected.size(); ++i)
                expected[i] = (block + i + 1) * 0x123456789abcdefULL;
            for (size_t i = 0; i < vectors.size(); ++i)
                vectors[i] = {i + block, ~std::uint64_t(i + block)};
            jit.SetRegisters(expected);
            jit.SetVectors(vectors);
            for (size_t round = 0; round < rounds; ++round) {
                for (size_t d = 0; d < expected.size(); ++d)
                    expected[d] = expected[(d + 1 + block % 29) % 31] + expected[(d + 7) % 31];
                for (size_t d = 0; d < vectors.size(); ++d)
                    for (size_t lane = 0; lane < 2; ++lane)
                        vectors[d][lane] =
                            vectors[(d + 1 + block % 29) % 32][lane] + vectors[(d + 7) % 32][lane];
            }
            expected[0] = expected[0] == expected[1] ? expected[1] : expected[2];
            jit.ClearHalt();
            jit.SetPC(block * 4096);
            jit.Run();
            Require(jit.GetRegisters() == expected, "GPR differential result");
            Require(jit.GetVectors() == vectors, "SIMD differential result");
            Require(env.svc_calls == block + 1 + (warm ? blocks : 0),
                    "SVC must execute exactly once");
        }
        return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    };
    const double cold = pass(false);
    const auto compilation_time = env.compilation_time;
    Require(env.compilations == blocks, "one cold compilation per block");
    const double warm = pass(true);
    Require(env.compilations == blocks, "warm execution must reuse compiled blocks");
    // Invalidation must rebuild the block and retain correct semantics.
    const auto before_invalidation = jit.GetRegisters();
    jit.InvalidateCacheRange(0, 4096);
    jit.ClearHalt();
    jit.SetPC(0);
    jit.Run();
    Require(env.compilations == blocks + 1, "range invalidation must recompile");
    auto expected = before_invalidation;
    for (size_t round = 0; round < rounds; ++round)
        for (size_t d = 0; d < expected.size(); ++d)
            expected[d] = expected[(d + 1) % 31] + expected[(d + 7) % 31];
    expected[0] = expected[0] == expected[1] ? expected[1] : expected[2];
    Require(jit.GetRegisters() == expected, "recompiled GPR differential result");
    std::printf("blocks,rounds,cold_ms,jit_compile_ms,warm_ms\n%zu,%zu,%.3f,%.3f,%.3f\n", blocks,
                rounds, cold, std::chrono::duration<double, std::milli>(compilation_time).count(),
                warm);
}
} // namespace
int main(int argc, char **argv) {
    const bool bench = argc > 1 && std::string_view(argv[1]) == "--benchmark";
    Run(bench ? 10000 : 100, 1, Dynarmic::all_safe_optimizations);
    Run(bench ? 10000 : 100, 6, Dynarmic::all_safe_optimizations);
    Run(100, 3, Dynarmic::no_optimizations);
}
