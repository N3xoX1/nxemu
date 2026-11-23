// SPDX-License-Identifier: GPL-2.0-or-later
#include "dynarmic/interface/A64/a64.h"
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {
using namespace Dynarmic;
using namespace Dynarmic::A64;

constexpr std::uint32_t NOP = 0xd503201f;
constexpr std::uint32_t SVC_0 = 0xd4000001;
constexpr std::uint32_t B_NEXT = 0x14000001;
constexpr std::uint32_t CBZ_X0_SKIP_ONE = 0xb4000040;
constexpr std::uint32_t MRS_X1_MIDR_EL1 = 0xd5380001;
constexpr std::uint32_t MRS_X1_CNTPCT_EL0 = 0xd53be021;
constexpr std::uint64_t X1_UNCHANGED = 0xdead;

void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct Environment final : UserCallbacks {
    Jit* jit{};
    std::vector<std::uint32_t> code;
    std::uint64_t ticks{};
    bool interpreted{};
    std::optional<std::uint32_t> MemoryReadCode(VAddr address) override {
        const size_t index = address / 4;
        return index < code.size() ? code[index] : SVC_0;
    }
    void CallSVC(std::uint32_t) override { jit->HaltExecution(); }
    void InterpreterFallback(VAddr, size_t) override {
        interpreted = true;
        jit->HaltExecution();
    }
    void ExceptionRaised(VAddr, Exception) override { Require(false, "unexpected exception"); }
    void AddTicks(std::uint64_t count) override { ticks += count; }
    std::uint64_t GetTicksRemaining() override { return 1000000; }
    std::uint64_t GetCNTPCT() override { return ticks; }
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

struct Result {
    std::uint64_t x1;
    bool interpreted;
};

Result Run(std::vector<std::uint32_t> code, std::uint64_t x0) {
    Environment env;
    env.code = std::move(code);
    UserConfig config{};
    config.callbacks = &env;
    config.code_cache_size = 64 * 1024 * 1024;
    Jit jit(config);
    env.jit = &jit;
    jit.SetRegister(0, x0);
    jit.SetRegister(1, X1_UNCHANGED);
    jit.SetPC(0);
    jit.Run();
    return {jit.GetRegister(1), env.interpreted};
}
} // namespace

int main() {
    const Result skipped = Run({CBZ_X0_SKIP_ONE, MRS_X1_MIDR_EL1, SVC_0}, 0);
    Require(!skipped.interpreted && skipped.x1 == X1_UNCHANGED,
            "a taken branch ran the instruction it skips");

    Require(Run({MRS_X1_CNTPCT_EL0, SVC_0}, 0).x1 == 0,
            "a counter read at the start counted cycles");
    Require(Run({NOP, MRS_X1_CNTPCT_EL0, SVC_0}, 0).x1 == 1,
            "a counter read missed the cycle of the NOP before it");
    Require(Run({B_NEXT, MRS_X1_CNTPCT_EL0, SVC_0}, 0).x1 == 1,
            "a counter read missed the cycle of the branch before it");

    std::puts("dynarmic side exit tests passed");
    return 0;
}
