// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Runs random MME programs through the production interpreter and the x64 macro JIT and
// compares every method call they make. The interpreter is the reference. Programs only branch
// forward, so they always end, and an epilogue sends all registers and the carry flag.
//
//   test_macro_jit_differential [programs] [seed] [--fuzz-only]
//   test_macro_jit_differential --benchmark

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "macro_jit_engines.h"
#include "yuzu_video_core/engines/maxwell_3d.h"

namespace {
using Tegra::Macro::ALUOperation;
using Tegra::Macro::BranchCondition;
using Tegra::Macro::Opcode;
using Tegra::Macro::Operation;
using Tegra::Macro::ResultOperation;
using Calls = std::vector<std::pair<u32, u32>>;

constexpr u32 EPILOGUE_METHOD = 0x200;

Opcode AddImmediate(ResultOperation result, u32 dst, u32 src, s32 immediate, bool exit = false) {
    Opcode op{};
    op.operation.Assign(Operation::AddImmediate);
    op.result_operation.Assign(result);
    op.dst.Assign(dst);
    op.src_a.Assign(src);
    op.immediate.Assign(immediate);
    op.is_exit.Assign(exit ? 1 : 0);
    return op;
}

Opcode Alu(ALUOperation alu, ResultOperation result, u32 dst, u32 src_a, u32 src_b) {
    Opcode op{};
    op.operation.Assign(Operation::ALU);
    op.result_operation.Assign(result);
    op.dst.Assign(dst);
    op.src_a.Assign(src_a);
    op.src_b.Assign(src_b);
    op.alu_operation.Assign(alu);
    return op;
}

Opcode Branch(BranchCondition condition, bool annul, u32 src, s32 offset) {
    Opcode op{};
    op.operation.Assign(Operation::Branch);
    op.branch_condition.Assign(condition);
    op.branch_annul.Assign(annul ? 1 : 0);
    op.src_a.Assign(src);
    op.immediate.Assign(offset);
    return op;
}

Opcode Read(ResultOperation result, u32 dst, u32 src, s32 immediate) {
    Opcode op = AddImmediate(result, dst, src, immediate);
    op.operation.Assign(Operation::Read);
    return op;
}

class Generator {
public:
    explicit Generator(u64 seed) : rng{seed} {}

    std::vector<u32> Program() {
        const u32 body_size = Uniform(1, 32);
        std::vector<Opcode> body;
        std::set<u32> not_targets;
        while (body.size() < body_size) {
            const bool after_delay = !body.empty() &&
                                     ((body.back().operation == Operation::Branch &&
                                       !body.back().branch_annul) ||
                                      body.back().is_exit);
            const u32 kind = Uniform(0, 99);
            if (kind < 14 && !after_delay) {
                // Target patched below, once the body size is known.
                body.push_back(Branch(static_cast<BranchCondition>(Uniform(0, 1)),
                                      Uniform(0, 2) == 0, Register(), 0));
                continue;
            }
            Opcode op{};
            if (kind < 44) {
                static constexpr ALUOperation alu_ops[]{
                    ALUOperation::Add, ALUOperation::AddWithCarry, ALUOperation::Subtract,
                    ALUOperation::SubtractWithBorrow, ALUOperation::Xor, ALUOperation::Or,
                    ALUOperation::And, ALUOperation::AndNot, ALUOperation::Nand};
                op = Alu(alu_ops[Uniform(0, 8)], Result(), Register(), Register(), Register());
            } else if (kind < 66) {
                op = AddImmediate(Result(), Register(), Register(), Immediate());
            } else if (kind < 76) {
                op.raw = static_cast<u32>(rng());
                op.operation.Assign(Operation::ExtractInsert);
            } else if (kind < 83) {
                op.raw = static_cast<u32>(rng());
                op.operation.Assign(Operation::ExtractShiftLeftImmediate);
            } else if (kind < 90) {
                op.raw = static_cast<u32>(rng());
                op.operation.Assign(Operation::ExtractShiftLeftRegister);
            } else if (Uniform(0, 1) == 0) {
                op = Read(Result(), Register(), 0, static_cast<s32>(Uniform(0, 0xDFF)));
            } else {
                // Keep register-relative reads in bounds: set the base right before the read,
                // and never branch to the read itself.
                const u32 base = Uniform(1, 7);
                body.push_back(AddImmediate(ResultOperation::Move, base, 0,
                                            static_cast<s32>(Uniform(0x400, 0x800))));
                not_targets.insert(static_cast<u32>(body.size()));
                op = Read(Result(), Register(), base, static_cast<s32>(Uniform(0, 0x800)) - 0x400);
                body.push_back(op);
                continue;
            }
            op.is_exit.Assign(Uniform(0, 24) == 0 ? 1 : 0);
            body.push_back(op);
        }
        const u32 end = static_cast<u32>(body.size());
        for (u32 pc = 0; pc < end; ++pc) {
            if (body[pc].operation != Operation::Branch) {
                continue;
            }
            u32 target;
            do {
                target = Uniform(pc + 1, end);
            } while (not_targets.contains(target));
            body[pc].immediate.Assign(static_cast<s32>(target - pc));
        }

        std::vector<u32> code;
        for (const Opcode& op : body) {
            code.push_back(op.raw);
        }
        // Epilogue: send r1-r7 to consecutive methods, optionally the carry flag, then exit.
        code.push_back(AddImmediate(ResultOperation::MoveAndSetMethod, 0, 0,
                                    static_cast<s32>(EPILOGUE_METHOD | (1U << 12)))
                           .raw);
        for (u32 reg = 1; reg < 8; ++reg) {
            code.push_back(AddImmediate(ResultOperation::MoveAndSend, 0, reg, 0).raw);
        }
        if (Uniform(0, 1) == 0) {
            code.push_back(Alu(ALUOperation::AddWithCarry, ResultOperation::MoveAndSend, 0, 0, 0).raw);
        }
        code.push_back(AddImmediate(ResultOperation::Move, 0, 0, 0, true).raw);
        code.push_back(AddImmediate(ResultOperation::Move, 0, 0, 0).raw);
        return code;
    }

    std::vector<u32> Parameters() {
        std::vector<u32> params(96);
        for (u32& value : params) {
            value = Value();
        }
        return params;
    }

    void FillRegisters(Tegra::Engines::Maxwell3D& maxwell3d) {
        for (u32& value : maxwell3d.front_regs.reg_array) {
            value = Value();
        }
    }

private:
    u32 Uniform(u32 low, u32 high) {
        return std::uniform_int_distribution<u32>{low, high}(rng);
    }
    u32 Register() {
        // Register 0 is the zero register: favour it, the JIT special-cases it.
        return Uniform(0, 3) == 0 ? 0 : Uniform(1, 7);
    }
    ResultOperation Result() {
        return static_cast<ResultOperation>(Uniform(0, 7));
    }
    s32 Immediate() {
        switch (Uniform(0, 3)) {
        case 0:
            return static_cast<s32>(Uniform(0, 4)) - 2;
        case 1:
            return static_cast<s32>(Uniform(0, 0x1FFF));
        default:
            return static_cast<s32>(Uniform(0, 0x3FFFF)) - 0x20000;
        }
    }
    u32 Value() {
        switch (Uniform(0, 3)) {
        case 0:
            return Uniform(0, 3);
        case 1:
            return 0xFFFFFFFFU - Uniform(0, 3);
        default:
            return static_cast<u32>(rng());
        }
    }

    std::mt19937_64 rng;
};

Calls Run(bool jit, const std::vector<u32>& code, const std::vector<u32>& params,
          const Tegra::Engines::Maxwell3D& registers) {
    Tegra::Engines::Maxwell3D maxwell3d = registers;
    const auto program = jit ? MakeTestJit(maxwell3d, code) : MakeTestInterpreter(maxwell3d, code);
    program->Execute(params, 0);
    return maxwell3d.calls;
}

void PrintCalls(const char* name, const Calls& calls) {
    std::fprintf(stderr, "  %s:", name);
    for (const auto& [method, value] : calls) {
        std::fprintf(stderr, " %03x=%08x", method, value);
    }
    std::fprintf(stderr, "\n");
}

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Cases the random programs cannot reach because the interpreter rejects them.
void DirectedCases() {
    Tegra::Engines::Maxwell3D maxwell3d;
    const std::vector<u32> params{1, 2, 3};
    for (const s32 offset : {1000, 0x1FFFF, -100, -0x20000}) {
        // A branch outside the macro ends it, also when the target is negative or past the
        // old fixed label table.
        const std::vector<u32> code{
            AddImmediate(ResultOperation::MoveAndSetMethod, 0, 0, 0x300).raw,
            AddImmediate(ResultOperation::MoveAndSend, 0, 1, 0).raw,
            Branch(BranchCondition::Zero, true, 0, offset).raw,
            AddImmediate(ResultOperation::MoveAndSend, 0, 0, 0x55).raw,
            AddImmediate(ResultOperation::Move, 0, 0, 0, true).raw,
            AddImmediate(ResultOperation::Move, 0, 0, 0).raw,
        };
        const Calls calls = Run(true, code, params, maxwell3d);
        Check(calls == Calls{{0x300, 1}}, "branch outside the macro ends it");
    }
    {
        // A branch to the end of the macro with a delay slot runs the slot, then ends.
        const std::vector<u32> code{
            AddImmediate(ResultOperation::MoveAndSetMethod, 0, 0, 0x300).raw,
            Branch(BranchCondition::Zero, false, 0, 2).raw,
            AddImmediate(ResultOperation::MoveAndSend, 0, 0, 0x11).raw,
        };
        const Calls calls = Run(true, code, params, maxwell3d);
        Check(calls == Calls{{0x300, 0x11}}, "delay slot before a branch to the end");
    }
}

bool Fuzz(u64 programs, u64 seed) {
    Generator generator{seed};
    u64 mismatches = 0;
    for (u64 index = 0; index < programs; ++index) {
        const std::vector<u32> code = generator.Program();
        const std::vector<u32> params = generator.Parameters();
        Tegra::Engines::Maxwell3D registers;
        generator.FillRegisters(registers);
        const Calls expected = Run(false, code, params, registers);
        const Calls actual = Run(true, code, params, registers);
        if (expected == actual) {
            continue;
        }
        if (++mismatches <= 5) {
            std::fprintf(stderr, "Mismatch in program %llu (seed %llu):\n  code:",
                         static_cast<unsigned long long>(index),
                         static_cast<unsigned long long>(seed));
            for (const u32 word : code) {
                std::fprintf(stderr, " %08x", word);
            }
            std::fprintf(stderr, "\n");
            PrintCalls("interpreter", expected);
            PrintCalls("jit", actual);
        }
    }
    std::fprintf(stderr, "%llu/%llu random programs differ\n",
                 static_cast<unsigned long long>(mismatches),
                 static_cast<unsigned long long>(programs));
    return mismatches == 0;
}

void Benchmark() {
    using Clock = std::chrono::steady_clock;
    Generator generator{12345};
    std::vector<std::vector<u32>> programs;
    for (int i = 0; i < 2000; ++i) {
        programs.push_back(generator.Program());
    }
    const std::vector<u32> params = generator.Parameters();
    Tegra::Engines::Maxwell3D maxwell3d;
    generator.FillRegisters(maxwell3d);

    std::vector<std::unique_ptr<Tegra::CachedMacro>> compiled;
    compiled.reserve(programs.size());
    const auto compile_start = Clock::now();
    for (const auto& code : programs) {
        compiled.push_back(MakeTestJit(maxwell3d, code));
    }
    const double compile_us =
        std::chrono::duration<double, std::micro>(Clock::now() - compile_start).count() /
        static_cast<double>(programs.size());

    constexpr int rounds = 200;
    const auto run_start = Clock::now();
    for (int round = 0; round < rounds; ++round) {
        for (const auto& program : compiled) {
            maxwell3d.calls.clear();
            program->Execute(params, 0);
        }
    }
    const double run_ns =
        std::chrono::duration<double, std::nano>(Clock::now() - run_start).count() /
        (static_cast<double>(programs.size()) * rounds);
    std::printf("compile_us=%.2f execute_ns=%.1f\n", compile_us, run_ns);
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0) {
        Benchmark();
        return 0;
    }
    const u64 programs = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000;
    const u64 seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
    if (argc <= 3 || std::strcmp(argv[3], "--fuzz-only") != 0) {
        DirectedCases();
    }
    const bool fuzz_ok = Fuzz(programs, seed);
    return failures == 0 && fuzz_ok ? 0 : 1;
}
