// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/fpsr_manager.h"

using namespace oaknut::util;
using Dynarmic::Backend::Arm64::FpsrManager;
using Dynarmic::Backend::Arm64::Xstate;

struct State {
    uint32_t fpsr;
    std::array<uint32_t, 5> reads{};
    uint32_t memory_after_read{};
};

// Execute both the old spill-before-read sequence and the production lazy reader.
void Emit(oaknut::CodeGenerator& code, bool lazy) {
    code.MOV(X14, Xstate);
    code.MRS(X15, oaknut::SystemReg::FPSR);
    code.MOV(Xstate, X0);
    FpsrManager manager{code, offsetof(State, fpsr)};
    const auto read = [&](size_t index) {
        if (lazy) {
            manager.GetFpsr(W3);
        } else {
            manager.Spill();
            code.LDR(W3, Xstate, offsetof(State, fpsr));
        }
        code.STR(W3, Xstate, offsetof(State, reads) + index * sizeof(uint32_t));
    };
    code.MSR(oaknut::SystemReg::FPSR, X1);
    read(0); // Unloaded: unrelated host flags must not enter the guest state.
    manager.Load();
    code.MSR(oaknut::SystemReg::FPSR, X1);
    read(1);
    code.LDR(W3, Xstate, offsetof(State, fpsr));
    code.STR(W3, Xstate, offsetof(State, memory_after_read));
    read(2); // Repeated reads are sticky.
    manager.Load();
    code.ORR(W1, W1, 0x08000000); // Add QC between reads.
    code.MSR(oaknut::SystemReg::FPSR, X1);
    read(3);
    manager.Spill();
    manager.Load();
    // Match A64SetFPSR: overwrite both memory and the live host register.
    code.STR(W2, Xstate, offsetof(State, fpsr));
    code.MSR(oaknut::SystemReg::FPSR, X2);
    read(4);
    manager.Spill();
    code.MSR(oaknut::SystemReg::FPSR, X15);
    code.MOV(Xstate, X14);
    code.RET();
}

int main() {
    oaknut::CodeBlock block{16384};
    block.unprotect();
    oaknut::CodeGenerator code{block.ptr()};
    auto* baseline = code.xptr<uint32_t*>();
    Emit(code, false);
    auto* optimized = code.xptr<uint32_t*>();
    Emit(code, true);
    block.invalidate_all();
    block.protect();
    using Function = void (*)(State*, uint32_t, uint32_t);
    const auto old_read = reinterpret_cast<Function>(baseline);
    const auto lazy_read = reinterpret_cast<Function>(optimized);
    // IOC, DZC, OFC, UFC, IXC, IDC, QC, individually and in every combination.
    constexpr std::array<uint32_t, 7> bits{1, 2, 4, 8, 16, 128, 1U << 27};
    for (uint32_t combination = 0; combination < (1U << bits.size()); ++combination) {
        uint32_t live = 0;
        for (size_t bit = 0; bit < bits.size(); ++bit) {
            if (combination & (1U << bit)) live |= bits[bit];
        }
        for (const uint32_t accumulated : {0U, 0x0800009fU, live ^ 0x0800009fU}) {
            const uint32_t overwrite = live ^ 0x0800009fU;
            State old_state{accumulated}, new_state{accumulated};
            old_read(&old_state, live, overwrite);
            lazy_read(&new_state, live, overwrite);
            assert(old_state.reads == new_state.reads);
            assert(new_state.reads[0] == accumulated);
            assert(new_state.reads[1] == (accumulated | live));
            assert(new_state.reads[2] == new_state.reads[1]);
            assert(new_state.reads[3] == (accumulated | live | 0x08000000U));
            assert(new_state.reads[4] == overwrite);
            assert(old_state.fpsr == new_state.fpsr && new_state.fpsr == overwrite);
            assert(new_state.memory_after_read == accumulated); // No intermediate store.
        }
    }
    std::puts("FPSR: 384 ARM64 differential cases passed");
}
