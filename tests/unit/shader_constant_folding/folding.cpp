// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/exception.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;
namespace Opt = Shader::Optimization;

// Constant integer folding must not inspect runtime shader state.
class Environment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32) override { throw std::runtime_error("ReadInstruction"); }
    u32 ReadCbufValue(u32, u32) override { throw std::runtime_error("ReadCbufValue"); }
    Shader::TextureType ReadTextureType(u32) override { throw std::runtime_error("ReadTextureType"); }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        throw std::runtime_error("ReadTexturePixelFormat");
    }
    bool IsTexturePixelFormatInteger(u32) override { throw std::runtime_error("TextureInteger"); }
    u32 ReadViewportTransformState() override { throw std::runtime_error("ViewportTransform"); }
    u32 TextureBoundBuffer() const override { throw std::runtime_error("TextureBoundBuffer"); }
    u32 LocalMemorySize() const override { throw std::runtime_error("LocalMemorySize"); }
    u32 SharedMemorySize() const override { throw std::runtime_error("SharedMemorySize"); }
    std::array<u32, 3> WorkgroupSize() const override { throw std::runtime_error("WorkgroupSize"); }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        throw std::runtime_error("GetReplaceConstBuffer");
    }
    void Dump(u64, u64) override { throw std::runtime_error("Dump"); }
};

void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Fixture {
    Shader::ObjectPool<IR::Inst> pool{32};
    IR::Block block{pool};
    IR::Program program;
    Environment env;
    std::vector<IR::Inst*> allocated;

    Fixture() {
        program.blocks.push_back(&block);
        program.post_order_blocks.push_back(&block);
    }

    ~Fixture() {
        // ObjectPool's union storage does not call the contained Inst destructors.
        // Include instructions removed by cleanup passes, not just the current block list.
        block.Instructions().clear();
        for (IR::Inst* inst : allocated) {
            std::destroy_at(inst);
        }
    }

    IR::Inst& Add(IR::Opcode opcode, std::initializer_list<IR::Value> args) {
        block.AppendNewInst(opcode, args);
        allocated.push_back(&block.back());
        return block.back();
    }

    IR::Inst& Reference(IR::Inst& inst) {
        return Add(IR::Opcode::Reference, {IR::Value{&inst}});
    }
};

constexpr std::array opcodes{IR::Opcode::BitwiseAnd32, IR::Opcode::BitwiseOr32,
                             IR::Opcode::BitwiseXor32, IR::Opcode::BitFieldUExtract,
                             IR::Opcode::BitFieldSExtract};
constexpr std::array unsupported{IR::Opcode::GetCarryFromOp, IR::Opcode::GetOverflowFromOp};
size_t folded_cases{};

void CheckFoldResult(Fixture& f, IR::Inst& inst, u32 expected, unsigned flags) {
    IR::Inst* zero = nullptr;
    IR::Inst* sign = nullptr;
    if (flags & 1) {
        zero = &f.Add(IR::Opcode::GetZeroFromOp, {IR::Value{&inst}});
        f.Reference(*zero);
    }
    if (flags & 2) {
        sign = &f.Add(IR::Opcode::GetSignFromOp, {IR::Value{&inst}});
        f.Reference(*sign);
    }
    auto& result = f.Reference(inst);
    Opt::ConstantPropagationPass(f.env, f.program);
    Check(result.Arg(0).IsImmediate(), "result was not folded");
    Check(result.Arg(0).U32() == expected, "incorrect folded result");
    if (zero) {
        Check(IR::Value{zero}.IsImmediate(), "zero flag was not folded");
        Check(IR::Value{zero}.U1() == (expected == 0), "incorrect zero flag");
    }
    if (sign) {
        Check(IR::Value{sign}.IsImmediate(), "sign flag was not folded");
        Check(IR::Value{sign}.U1() == (expected >= 0x80000000U), "incorrect sign flag");
    }
    Check(!inst.GetAssociatedPseudoOperation(IR::Opcode::GetZeroFromOp) &&
              !inst.GetAssociatedPseudoOperation(IR::Opcode::GetSignFromOp),
          "stale pseudo-operation association");
    Check(inst.UseCount() == 1, "stale use count after flag folding");
    Opt::VerificationPass(f.program);
    Opt::IdentityRemovalPass(f.program);
    Opt::DeadCodeEliminationPass(f.program);
    Opt::VerificationPass(f.program);
    Check(result.Arg(0).U32() == expected, "cleanup changed the result");
    ++folded_cases;
}

void CheckFold(IR::Opcode opcode, std::initializer_list<IR::Value> args, u32 expected,
               unsigned flags) {
    Fixture f;
    auto& inst = f.Add(opcode, args);
    CheckFoldResult(f, inst, expected, flags);
}

std::vector<u32> Inputs() {
    std::vector<u32> values{0, 1, 0x7fffffffU, 0x80000000U, 0xffffffffU,
                            0x55555555U, 0xaaaaaaaaU};
    for (u32 bit = 0; bit < 32; ++bit) {
        values.push_back(1U << bit);
    }
    u32 state = 0x12345678U;
    for (unsigned i = 0; i < 32; ++i) {
        state = state * 1664525U + 1013904223U;
        values.push_back(state);
    }
    return values;
}

void BitFields(bool is_signed) {
    for (const u32 base : Inputs()) {
        for (u32 shift = 0; shift <= 32; ++shift) {
            for (u32 count = 0; count <= 32 - shift; ++count) {
                // Independent, bit-by-bit reference to avoid repeating the folding formula.
                u32 expected = 0;
                for (u32 bit = 0; bit < count; ++bit) {
                    if ((static_cast<u64>(base) >> (shift + bit)) & 1) {
                        expected |= 1U << bit;
                    }
                }
                if (is_signed && count > 0 && ((expected >> (count - 1)) & 1)) {
                    for (u32 bit = count; bit < 32; ++bit) {
                        expected |= 1U << bit;
                    }
                }
                for (unsigned flags = 0; flags < 4; ++flags) {
                    CheckFold(is_signed ? IR::Opcode::BitFieldSExtract : IR::Opcode::BitFieldUExtract,
                              {IR::Value{base}, IR::Value{shift}, IR::Value{count}}, expected, flags);
                }
            }
        }
    }
}

void Logical() {
    for (const u32 a : Inputs()) {
        for (const u32 b : Inputs()) {
            for (unsigned flags = 0; flags < 4; ++flags) {
                CheckFold(IR::Opcode::BitwiseAnd32, {IR::Value{a}, IR::Value{b}}, a & b, flags);
                CheckFold(IR::Opcode::BitwiseOr32, {IR::Value{a}, IR::Value{b}}, a | b, flags);
                CheckFold(IR::Opcode::BitwiseXor32, {IR::Value{a}, IR::Value{b}}, a ^ b, flags);
            }
        }
    }
}

IR::Value AlgebraicRhs(IR::Opcode opcode, IR::Inst& dynamic) {
    switch (opcode) {
    case IR::Opcode::BitwiseAnd32:
        return IR::Value{0U};
    case IR::Opcode::BitwiseOr32:
        return IR::Value{0xffffffffU};
    default:
        return IR::Value{&dynamic};
    }
}

void AlgebraicIdentities() {
    for (const auto opcode : {IR::Opcode::BitwiseAnd32, IR::Opcode::BitwiseOr32,
                              IR::Opcode::BitwiseXor32}) {
        for (unsigned flags = 0; flags < 4; ++flags) {
            for (bool reverse : {false, true}) {
                for (unsigned aliases = 0; aliases < 3; ++aliases) {
                    Fixture f;
                    auto& dynamic = f.Add(IR::Opcode::GetCbufU32, {IR::Value{0U}, IR::Value{16U}});
                    IR::Value lhs{&dynamic};
                    IR::Value rhs = AlgebraicRhs(opcode, dynamic);
                    // Distinct Identity nodes still refer to the same SSA definition.
                    for (unsigned i = 0; i < aliases; ++i) {
                        lhs = IR::Value{&f.Add(IR::Opcode::Identity, {lhs})};
                        rhs = IR::Value{&f.Add(IR::Opcode::Identity, {rhs})};
                    }
                    auto& inst = reverse ? f.Add(opcode, {rhs, lhs}) : f.Add(opcode, {lhs, rhs});
                    CheckFoldResult(f, inst, opcode == IR::Opcode::BitwiseOr32 ? 0xffffffffU : 0U,
                                    flags);
                }
            }
        }
        for (const auto pseudo : unsupported) {
            Fixture f;
            auto& dynamic = f.Add(IR::Opcode::GetCbufU32, {IR::Value{0U}, IR::Value{16U}});
            auto& inst = f.Add(opcode, {IR::Value{&dynamic}, AlgebraicRhs(opcode, dynamic)});
            auto& zero = f.Add(IR::Opcode::GetZeroFromOp, {IR::Value{&inst}});
            auto& sign = f.Add(IR::Opcode::GetSignFromOp, {IR::Value{&inst}});
            auto& other = f.Add(pseudo, {IR::Value{&inst}});
            Opt::ConstantPropagationPass(f.env, f.program);
            Check(inst.GetOpcode() == opcode, "algebraic folding changed an unsupported flag");
            Check(zero.GetOpcode() == IR::Opcode::GetZeroFromOp, "partially folded zero flag");
            Check(sign.GetOpcode() == IR::Opcode::GetSignFromOp, "partially folded sign flag");
            Check(other.GetOpcode() == pseudo, "changed unsupported flag");
            Opt::VerificationPass(f.program);
        }
    }
}

void PreserveDistinctSsaValues() {
    for (unsigned flags = 0; flags < 4; ++flags) {
        Fixture f;
        auto& lhs = f.Add(IR::Opcode::GetCbufU32, {IR::Value{0U}, IR::Value{16U}});
        auto& rhs = f.Add(IR::Opcode::GetCbufU32, {IR::Value{0U}, IR::Value{16U}});
        auto& inst = f.Add(IR::Opcode::BitwiseXor32, {IR::Value{&lhs}, IR::Value{&rhs}});
        if (flags & 1) {
            f.Add(IR::Opcode::GetZeroFromOp, {IR::Value{&inst}});
        }
        if (flags & 2) {
            f.Add(IR::Opcode::GetSignFromOp, {IR::Value{&inst}});
        }
        f.Reference(inst);
        Opt::ConstantPropagationPass(f.env, f.program);
        Check(inst.GetOpcode() == IR::Opcode::BitwiseXor32, "conflated distinct SSA definitions");
        Opt::VerificationPass(f.program);
    }
    Fixture f;
    auto& undefined = f.Add(IR::Opcode::UndefU32, {});
    auto& inst = f.Add(IR::Opcode::BitwiseXor32, {IR::Value{&undefined}, IR::Value{&undefined}});
    Opt::ConstantPropagationPass(f.env, f.program);
    Check(inst.GetOpcode() == IR::Opcode::BitwiseXor32, "assumed correlated undefined values");
    Opt::VerificationPass(f.program);
}

void PreserveUnsupportedFlagsAndDynamicOperands() {
    for (const auto opcode : opcodes) {
        // Also verify that no Z/S flag is partially replaced when another flag blocks folding.
        for (const auto pseudo : unsupported) {
            Fixture f;
            auto& inst = opcode == IR::Opcode::BitFieldUExtract || opcode == IR::Opcode::BitFieldSExtract
                             ? f.Add(opcode, {IR::Value{0x80000000U}, IR::Value{0U}, IR::Value{32U}})
                             : f.Add(opcode, {IR::Value{0x80000000U}, IR::Value{0U}});
            auto& zero = f.Add(IR::Opcode::GetZeroFromOp, {IR::Value{&inst}});
            auto& sign = f.Add(IR::Opcode::GetSignFromOp, {IR::Value{&inst}});
            auto& other = f.Add(pseudo, {IR::Value{&inst}});
            Opt::ConstantPropagationPass(f.env, f.program);
            Check(inst.GetOpcode() == opcode, "folded instruction with unsupported flag");
            Check(zero.GetOpcode() == IR::Opcode::GetZeroFromOp, "partially folded zero flag");
            Check(sign.GetOpcode() == IR::Opcode::GetSignFromOp, "partially folded sign flag");
            Check(other.GetOpcode() == pseudo, "changed unsupported flag");
            Opt::VerificationPass(f.program);
        }
        const unsigned num_args = opcode == IR::Opcode::BitFieldUExtract ||
                                          opcode == IR::Opcode::BitFieldSExtract ? 3 : 2;
        for (unsigned dynamic_arg = 0; dynamic_arg < num_args; ++dynamic_arg) {
            Fixture f;
            auto& dynamic = f.Add(IR::Opcode::UndefU32, {});
            std::array args{IR::Value{1U}, IR::Value{2U}, IR::Value{1U}};
            args[dynamic_arg] = IR::Value{&dynamic};
            auto& inst = num_args == 3 ? f.Add(opcode, {args[0], args[1], args[2]})
                                      : f.Add(opcode, {args[0], args[1]});
            auto& zero = f.Add(IR::Opcode::GetZeroFromOp, {IR::Value{&inst}});
            auto& sign = f.Add(IR::Opcode::GetSignFromOp, {IR::Value{&inst}});
            Opt::ConstantPropagationPass(f.env, f.program);
            Check(inst.GetOpcode() == opcode, "folded a runtime operand");
            Check(zero.GetOpcode() == IR::Opcode::GetZeroFromOp, "changed runtime zero flag");
            Check(sign.GetOpcode() == IR::Opcode::GetSignFromOp, "changed runtime sign flag");
            Opt::VerificationPass(f.program);
        }
    }
}

void InvalidBitFields() {
    constexpr std::array invalid{std::array{33U, 0U}, std::array{0U, 33U},
                                  std::array{31U, 2U}, std::array{32U, 1U},
                                  std::array{0xffffffffU, 1U}, std::array{1U, 0xffffffffU}};
    for (const auto opcode : {IR::Opcode::BitFieldUExtract, IR::Opcode::BitFieldSExtract}) {
        for (const auto [shift, count] : invalid) {
            Fixture f;
            f.Add(opcode, {IR::Value{0xffffffffU}, IR::Value{shift}, IR::Value{count}});
            bool rejected = false;
            try {
                Opt::ConstantPropagationPass(f.env, f.program);
            } catch (const Shader::LogicError&) {
                rejected = true;
            }
            Check(rejected, "accepted an invalid bitfield range");
        }
    }
}

void ResolveHandleSelection() {
    Fixture f;
    auto& field = f.Add(IR::Opcode::BitFieldSExtract,
                        {IR::Value{0x80000000U}, IR::Value{31U}, IR::Value{1U}});
    auto& zero = f.Add(IR::Opcode::GetZeroFromOp, {IR::Value{&field}});
    auto& sign = f.Add(IR::Opcode::GetSignFromOp, {IR::Value{&field}});
    auto& nonzero = f.Add(IR::Opcode::LogicalNot, {IR::Value{&zero}});
    auto& predicate = f.Add(IR::Opcode::LogicalAnd, {IR::Value{&sign}, IR::Value{&nonzero}});
    auto& chosen = f.Add(IR::Opcode::GetCbufU32, {IR::Value{1U}, IR::Value{32U}});
    auto& other = f.Add(IR::Opcode::GetCbufU32, {IR::Value{2U}, IR::Value{64U}});
    auto& selected = f.Add(IR::Opcode::SelectU32,
                           {IR::Value{&predicate}, IR::Value{&chosen}, IR::Value{&other}});
    auto& result = f.Reference(selected);
    Opt::ConstantPropagationPass(f.env, f.program);
    Check(result.Arg(0).Resolve() == IR::Value{&chosen}, "handle selection remained dynamic");
    Opt::IdentityRemovalPass(f.program);
    Opt::DeadCodeEliminationPass(f.program);
    Opt::VerificationPass(f.program);
    Check(result.Arg(0) == IR::Value{&chosen}, "cleanup changed the selected handle");
}
} // namespace

int main() {
    const std::array<std::pair<const char*, void (*)()>, 8> tests{{
        {"unsigned bitfields", [] { BitFields(false); }},
        {"signed bitfields", [] { BitFields(true); }},
        {"logical flags", Logical},
        {"algebraic identities with runtime operands", AlgebraicIdentities},
        {"distinct SSA and undefined values", PreserveDistinctSsaValues},
        {"unsupported flags and runtime operands", PreserveUnsupportedFlagsAndDynamicOperands},
        {"invalid bitfield ranges", InvalidBitFields},
        {"handle selection through folded flags", ResolveHandleSelection},
    }};
    int failures = 0;
    for (const auto& [name, run] : tests) {
        try {
            run();
            std::printf("PASS %s\n", name);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
            ++failures;
        }
    }
    std::printf("%zu constant folds checked\n", folded_cases);
    return failures ? 1 : 0;
}
