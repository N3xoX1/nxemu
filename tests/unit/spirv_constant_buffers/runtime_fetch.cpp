// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/exception.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_shader_recompiler/ir_opt/conditional_value.h"

namespace {
class FetchEnvironment final : public Shader::Environment {
public:
    FetchEnvironment() { stage = Shader::Stage::Fragment; }
    bool has_pool = true;
    u64 ReadInstruction(u32) override { return 0; }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    std::optional<Shader::TexturePoolInfo> ReadTexturePoolInfo() override {
        if (!has_pool) return std::nullopt;
        return Shader::TexturePoolInfo{16, 4, false};
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 2; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

void ConstantBitfieldFlags() {
    using namespace Shader;
    for (const u32 base : {0U, 7U, 8U, 0x80000000U, 0xFFFFFFFFU}) {
        for (u32 shift = 0; shift <= 32; ++shift) {
            for (u32 count = 0; count <= 32 - shift; ++count) {
                ObjectPool<IR::Inst> pool;
                IR::Block block{pool};
                IR::IREmitter ir{block};
                const auto value = ir.BitFieldExtract(ir.Imm32(base), ir.Imm32(shift), ir.Imm32(count), false);
                const auto zero = ir.GetZeroFromOp(value);
                const auto sign = ir.GetSignFromOp(value);
                IR::Program program{};
                program.blocks = {&block};
                program.post_order_blocks = {&block};
                FetchEnvironment env;
                try {
                    Optimization::ConstantPropagationPass(env, program);
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "base=%u shift=%u count=%u: %s\n", base, shift, count, error.what());
                    throw;
                }
                // Independent bit-by-bit reference, including zero/full-width cases.
                u32 expected{};
                for (u32 bit = 0; bit < count; ++bit)
                    expected |= ((base >> (shift + bit)) & 1U) << bit;
                assert(value.Resolve().IsImmediate() && value.Resolve().U32() == expected);
                assert(zero.Resolve().IsImmediate() && zero.Resolve().U1() == (expected == 0));
                assert(sign.Resolve().IsImmediate() && sign.Resolve().U1() == ((expected >> 31) != 0));
            }
        }
    }
}

void FlaggedPredicateHandle(bool propagate) {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, sampled{pool}, end{pool};
    IR::IREmitter ir{entry}, body{sampled};
    const auto predicate = ir.INotEqual(ir.GetAttributeU32(IR::Attribute::Generic0X), ir.Imm32(0));
    const auto source = ir.GetCbuf(ir.Imm32(2), ir.Imm32(384));
    const auto handle = ir.Select(predicate, source, ir.Imm32(0xDEADBEEF));
    // Captured Nikoderiko pattern: BFE(7, 3, 1) is zero, but its live zero flag
    // used to prevent folding. The repeated predicate then appeared unrelated.
    const auto extracted = ir.BitFieldExtract(ir.Imm32(7), ir.Imm32(3), ir.Imm32(1), false);
    const IR::U1 selected{ir.Select(ir.GetZeroFromOp(extracted), predicate, ir.Imm1(false))};
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Color2D);
    const auto sample = body.ImageSampleImplicitLod(handle,
        body.CompositeConstruct(body.Imm32(0.0f), body.Imm32(0.0f)), body.Imm32(0.0f), {}, {}, flags);
    IR::Program program{};
    program.stage = Stage::Fragment;
    program.blocks = {&entry, &sampled, &end};
    program.post_order_blocks = {&end, &sampled, &entry};
    using Node = IR::AbstractSyntaxNode;
    program.syntax_list = {
        {.data = {.block = &entry}, .type = Node::Type::Block},
        {.data = {.if_node = {selected, &sampled, &end}}, .type = Node::Type::If},
        {.data = {.block = &sampled}, .type = Node::Type::Block},
        {.data = {.end_if = {&end}}, .type = Node::Type::EndIf},
        {.data = {.block = &end}, .type = Node::Type::Block},
    };
    FetchEnvironment env;
    HostTranslateInfo host{};
    host.support_separate_sampler = true;
    host.max_bindless_descriptors_per_stage = 32;
    if (propagate) Optimization::ConstantPropagationPass(env, program);
    Optimization::TexturePass(env, program, host);
    assert(sample.Inst()->GetOpcode() == IR::Opcode::ImageSampleImplicitLod);
    assert(program.info.texture_descriptors.size() == 1);
    const auto& descriptor = program.info.texture_descriptors.front();
    assert(descriptor.is_direct == !propagate);
    assert(descriptor.count == (propagate ? 1U : 16U));
    if (propagate) assert(descriptor.cbuf_index == 2 && descriptor.cbuf_offset == 384);
}

void RuntimeFetch(bool has_pool, u32 budget, bool succeeds) {
    Shader::ObjectPool<Shader::IR::Inst> pool;
    Shader::IR::Block block{pool};
    Shader::IR::IREmitter ir{block};
    // A runtime input has no constant-buffer address. Preserve its original handle.
    const auto handle = ir.GetAttributeU32(Shader::IR::Attribute::Generic0X);
    Shader::IR::TextureInstInfo flags{};
    flags.type.Assign(Shader::TextureType::Color2D);
    const auto fetch = ir.ImageFetch(handle, ir.CompositeConstruct(ir.Imm32(0), ir.Imm32(0)),
                                    {}, ir.Imm32(0), {}, flags);
    Shader::IR::Program program{};
    program.stage = Shader::Stage::Fragment;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    FetchEnvironment env;
    env.has_pool = has_pool;
    Shader::HostTranslateInfo host{};
    host.support_separate_sampler = true;
    host.max_bindless_descriptors_per_stage = budget;
    try {
        Shader::Optimization::TexturePass(env, program, host);
        assert(succeeds);
        assert(fetch.Inst()->GetOpcode() == Shader::IR::Opcode::ImageFetch);
        assert(program.info.texture_descriptors.size() == 1);
        const auto& desc = program.info.texture_descriptors.front();
        assert(desc.is_direct && desc.count == 16);
        assert(program.info.runtime_sampler_count == 4);
        assert(!fetch.Inst()->Arg(0).IsImmediate());
    } catch (const Shader::NotImplementedException&) {
        assert(!succeeds);
    }
}

void ImpossibleUniformFetch(bool same_word, bool uniform = true) {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, inner{pool}, dead{pool}, merge{pool}, end{pool};
    IR::IREmitter outer_ir{entry}, inner_ir{inner}, ir{dead};
    const auto first = uniform ? outer_ir.GetCbuf(outer_ir.Imm32(3), outer_ir.Imm32(4))
                               : outer_ir.GetAttributeU32(IR::Attribute::Generic0X);
    const auto outer = outer_ir.ILessThan(first,
        outer_ir.GetCbuf(outer_ir.Imm32(3), outer_ir.Imm32(12)), false);
    const auto second = uniform
        ? inner_ir.GetCbuf(inner_ir.Imm32(3), inner_ir.Imm32(same_word ? 4 : 8)) : first;
    const auto opposite = inner_ir.IGreaterThanEqual(
        second,
        inner_ir.GetCbuf(inner_ir.Imm32(3), inner_ir.Imm32(12)), false);
    const auto handle = ir.GetAttributeU32(IR::Attribute::Generic0X);
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Color2D);
    const auto fetch = ir.ImageFetch(handle, ir.CompositeConstruct(ir.Imm32(0), ir.Imm32(0)),
                                    {}, ir.Imm32(0), {}, flags);
    IR::Program program{};
    program.stage = Stage::Fragment;
    program.blocks = {&entry, &inner, &dead, &merge, &end};
    program.post_order_blocks = {&end, &merge, &dead, &inner, &entry};
    using Node = IR::AbstractSyntaxNode;
    program.syntax_list = {
        {.data = {.block = &entry}, .type = Node::Type::Block},
        {.data = {.if_node = {outer, &inner, &end}}, .type = Node::Type::If},
        {.data = {.block = &inner}, .type = Node::Type::Block},
        {.data = {.if_node = {opposite, &dead, &merge}}, .type = Node::Type::If},
        {.data = {.block = &dead}, .type = Node::Type::Block},
        {.data = {.end_if = {&merge}}, .type = Node::Type::EndIf},
        {.data = {.block = &merge}, .type = Node::Type::Block},
        {.data = {.end_if = {&end}}, .type = Node::Type::EndIf},
        {.data = {.block = &end}, .type = Node::Type::Block},
    };
    Optimization::ConditionalValueResolver conditions{program};
    conditions.BeginBlock(&dead);
    const bool impossible = same_word && uniform;
    assert(conditions.HasContradictoryUniformConditions() == impossible);
    FetchEnvironment env;
    env.has_pool = false;
    HostTranslateInfo host{};
    host.support_separate_sampler = true;
    host.max_bindless_descriptors_per_stage = 8;
    bool translated = false;
    try {
        Optimization::TexturePass(env, program, host);
        translated = true;
    } catch (const NotImplementedException&) {}
    assert(translated == impossible);
    if (translated) {
        assert(fetch.Inst()->GetOpcode() == IR::Opcode::Identity);
        assert(program.info.texture_descriptors.empty());
        assert(program.info.runtime_sampler_count == 0);
    }
}
} // namespace

void TestRuntimeTextureFetch() {
    ConstantBitfieldFlags();
    FlaggedPredicateHandle(false);
    FlaggedPredicateHandle(true);
    RuntimeFetch(true, 32, false); // Reachable unknown handles must not allocate the full TIC table.
    RuntimeFetch(false, 32, false);
    RuntimeFetch(true, 8, false);
    ImpossibleUniformFetch(true);
    ImpossibleUniformFetch(false);
    ImpossibleUniformFetch(true, false); // SSA/loop-varying predicates stay conservative.
}
