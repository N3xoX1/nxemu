// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
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
                Optimization::ConstantPropagationPass(env, program);
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

void FlaggedPredicateHandle(bool propagate, bool merged_predicate = false) {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, left{pool}, merge{pool}, left_again{pool}, merge_again{pool}, sampled{pool}, end{pool};
    IR::IREmitter ir{entry}, left_ir{left}, body{sampled};
    const auto predicate = ir.INotEqual(ir.GetAttributeU32(IR::Attribute::Generic0X), ir.Imm32(0));
    const auto source = ir.GetCbuf(ir.Imm32(2), ir.Imm32(384));
    const auto handle = ir.Select(predicate, source, ir.Imm32(0xDEADBEEF));
    // Captured Nikoderiko pattern: BFE(7, 3, 1) is zero, but its live zero flag
    // used to prevent folding. The repeated predicate then appeared unrelated.
    auto& predicate_ir = merged_predicate ? left_ir : ir;
    const auto extracted = predicate_ir.BitFieldExtract(ir.Imm32(7), ir.Imm32(3), ir.Imm32(1), false);
    const IR::U1 selected{predicate_ir.Select(predicate_ir.GetZeroFromOp(extracted), predicate, ir.Imm1(false))};
    IR::U1 condition = selected;
    if (merged_predicate) {
        auto first = merge.PrependNewInst(merge.end(), IR::Opcode::Phi, {}, static_cast<u32>(IR::Type::U1));
        first->AddPhiOperand(&entry, predicate);
        first->AddPhiOperand(&left, selected);
        auto second = merge_again.PrependNewInst(merge_again.end(), IR::Opcode::Phi, {}, static_cast<u32>(IR::Type::U1));
        second->AddPhiOperand(&merge, IR::Value{&*first});
        second->AddPhiOperand(&left_again, IR::Value{&*first});
        condition = IR::U1{IR::Value{&*second}};
    }
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
        {.data = {.if_node = {condition, &sampled, &end}}, .type = Node::Type::If},
        {.data = {.block = &sampled}, .type = Node::Type::Block},
        {.data = {.end_if = {&end}}, .type = Node::Type::EndIf},
        {.data = {.block = &end}, .type = Node::Type::Block},
    };
    if (merged_predicate) {
        program.blocks = {&entry, &left, &merge, &left_again, &merge_again, &sampled, &end};
        program.post_order_blocks = {&end, &sampled, &merge_again, &left_again, &merge, &left, &entry};
        program.syntax_list = {
            {.data = {.block = &entry}, .type = Node::Type::Block},
            {.data = {.if_node = {predicate, &left, &merge}}, .type = Node::Type::If},
            {.data = {.block = &left}, .type = Node::Type::Block},
            {.data = {.end_if = {&merge}}, .type = Node::Type::EndIf},
            {.data = {.block = &merge}, .type = Node::Type::Block},
            {.data = {.if_node = {predicate, &left_again, &merge_again}}, .type = Node::Type::If},
            {.data = {.block = &left_again}, .type = Node::Type::Block},
            {.data = {.end_if = {&merge_again}}, .type = Node::Type::EndIf},
            {.data = {.block = &merge_again}, .type = Node::Type::Block},
            {.data = {.if_node = {condition, &sampled, &end}}, .type = Node::Type::If},
            {.data = {.block = &sampled}, .type = Node::Type::Block},
            {.data = {.end_if = {&end}}, .type = Node::Type::EndIf},
            {.data = {.block = &end}, .type = Node::Type::Block},
        };
    }
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
    if (merged_predicate) {
        assert(merge.front().GetOpcode() == IR::Opcode::Phi);
        assert(merge_again.front().GetOpcode() == IR::Opcode::Phi);
    }
}

void PredicatePhiFacts() {
    using namespace Shader;
    for (unsigned mode = 0; mode < 4; ++mode) {
        ObjectPool<IR::Inst> pool;
        IR::Block entry{pool}, header{pool}, body{pool}, chosen{pool}, latch{pool}, end{pool};
        IR::IREmitter ir{entry};
        const auto original = ir.INotEqual(ir.GetAttributeU32(IR::Attribute::Generic0X), ir.Imm32(0));
        const auto other = ir.INotEqual(ir.GetAttributeU32(IR::Attribute::Generic0Y), ir.Imm32(0));
        auto phi = header.PrependNewInst(header.end(), IR::Opcode::Phi, {}, static_cast<u32>(IR::Type::U1));
        phi->AddPhiOperand(&entry, original);
        IR::Value incoming = original;
        if (mode == 1) incoming = other;
        if (mode == 2) incoming = IR::Value{&*phi};
        if (mode == 3) incoming = IR::Value{&*latch.PrependNewInst(latch.end(), IR::Opcode::UndefU1)};
        phi->AddPhiOperand(&latch, incoming);
        const IR::U1 condition{IR::Value{&*phi}};
        IR::Program program{};
        program.blocks = {&entry, &header, &body, &chosen, &latch, &end};
        using Node = IR::AbstractSyntaxNode;
        program.syntax_list = {
            {.data = {.block = &entry}, .type = Node::Type::Block},
            {.data = {.block = &header}, .type = Node::Type::Block},
            {.data = {.loop = {&body, &latch, &end}}, .type = Node::Type::Loop},
            {.data = {.block = &body}, .type = Node::Type::Block},
            {.data = {.if_node = {condition, &chosen, &latch}}, .type = Node::Type::If},
            {.data = {.block = &chosen}, .type = Node::Type::Block},
            {.data = {.end_if = {&latch}}, .type = Node::Type::EndIf},
            {.data = {.block = &latch}, .type = Node::Type::Block},
            {.data = {.repeat = {ir.Imm1(false), &header, &end}}, .type = Node::Type::Repeat},
            {.data = {.block = &end}, .type = Node::Type::Block},
        };
        Optimization::ConditionalValueResolver resolver{program};
        resolver.BeginBlock(&chosen);
        assert(resolver.CrossesLoop(&*phi));
        const auto truth = resolver.Truth(original);
        if (mode == 0) assert(truth && *truth);
        else assert(!truth);
        assert(phi->GetOpcode() == IR::Opcode::Phi && phi->NumArgs() == 2);
        assert(phi->Arg(0) == original && phi->Arg(1) == incoming);
    }
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

void ContradictoryOuterPredicateHandle() {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, outer_body{pool}, load_tex{pool}, merge_tex{pool},
        or_samp{pool}, merge_samp{pool}, fetch_body{pool}, merge_fetch{pool}, end{pool};
    IR::IREmitter entry_ir{entry}, body_ir{outer_body}, tex_ir{load_tex}, samp_ir{or_samp},
        fetch_ir{fetch_body};
    const auto a = entry_ir.GetAttributeU32(IR::Attribute::Generic0X);
    const auto b = entry_ir.GetAttributeU32(IR::Attribute::Generic0Y);
    const auto stale = IR::U32{IR::Value{&*entry.PrependNewInst(entry.end(), IR::Opcode::UndefU32)}};
    const auto outer_cond = entry_ir.LogicalNot(entry_ir.IGreaterThanEqual(a, b, false));
    // Re-emitted opposite comparison inside outer_body, matching Maxwell predicated loop peels.
    const auto inner_cond = body_ir.IGreaterThanEqual(a, b, false);
    const auto tex_handle = tex_ir.GetCbuf(tex_ir.Imm32(2), tex_ir.Imm32(360));
    auto phi_tex = merge_tex.PrependNewInst(merge_tex.end(), IR::Opcode::Phi, {}, static_cast<u32>(IR::Type::U32));
    phi_tex->AddPhiOperand(&outer_body, stale);
    phi_tex->AddPhiOperand(&load_tex, tex_handle);
    const auto combined = samp_ir.BitwiseOr(IR::U32{IR::Value{&*phi_tex}},
                                            samp_ir.GetCbuf(samp_ir.Imm32(2), samp_ir.Imm32(1392)));
    auto phi_combined = merge_samp.PrependNewInst(merge_samp.end(), IR::Opcode::Phi, {}, static_cast<u32>(IR::Type::U32));
    phi_combined->AddPhiOperand(&merge_tex, IR::Value{&*phi_tex});
    phi_combined->AddPhiOperand(&or_samp, combined);
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Color2D);
    const auto fetch = fetch_ir.ImageFetch(IR::U32{IR::Value{&*phi_combined}},
        fetch_ir.CompositeConstruct(fetch_ir.Imm32(0), fetch_ir.Imm32(0)), {}, fetch_ir.Imm32(0), {}, flags);
    IR::Program program{};
    program.stage = Stage::Compute;
    program.blocks = {&entry, &outer_body, &load_tex, &merge_tex, &or_samp, &merge_samp,
                      &fetch_body, &merge_fetch, &end};
    program.post_order_blocks = {&end, &merge_fetch, &fetch_body, &merge_samp, &or_samp,
                                 &merge_tex, &load_tex, &outer_body, &entry};
    using Node = IR::AbstractSyntaxNode;
    program.syntax_list = {
        {.data = {.block = &entry}, .type = Node::Type::Block},
        {.data = {.if_node = {outer_cond, &outer_body, &end}}, .type = Node::Type::If},
        {.data = {.block = &outer_body}, .type = Node::Type::Block},
        {.data = {.if_node = {inner_cond, &load_tex, &merge_tex}}, .type = Node::Type::If},
        {.data = {.block = &load_tex}, .type = Node::Type::Block},
        {.data = {.end_if = {&merge_tex}}, .type = Node::Type::EndIf},
        {.data = {.block = &merge_tex}, .type = Node::Type::Block},
        {.data = {.if_node = {inner_cond, &or_samp, &merge_samp}}, .type = Node::Type::If},
        {.data = {.block = &or_samp}, .type = Node::Type::Block},
        {.data = {.end_if = {&merge_samp}}, .type = Node::Type::EndIf},
        {.data = {.block = &merge_samp}, .type = Node::Type::Block},
        {.data = {.if_node = {inner_cond, &fetch_body, &merge_fetch}}, .type = Node::Type::If},
        {.data = {.block = &fetch_body}, .type = Node::Type::Block},
        {.data = {.end_if = {&merge_fetch}}, .type = Node::Type::EndIf},
        {.data = {.block = &merge_fetch}, .type = Node::Type::Block},
        {.data = {.end_if = {&end}}, .type = Node::Type::EndIf},
        {.data = {.block = &end}, .type = Node::Type::Block},
    };
    FetchEnvironment env;
    env.has_pool = false;
    HostTranslateInfo host{};
    host.support_separate_sampler = true;
    host.max_bindless_descriptors_per_stage = 32;
    Optimization::TexturePass(env, program, host);
    assert(fetch.Inst()->GetOpcode() == IR::Opcode::ImageFetch);
    assert(program.info.texture_descriptors.size() == 1);
    const auto& desc = program.info.texture_descriptors.front();
    assert(!desc.is_direct && desc.count == 1);
    assert(desc.cbuf_index == 2 && desc.cbuf_offset == 360);
    assert(desc.has_secondary && desc.secondary_cbuf_index == 2 && desc.secondary_cbuf_offset == 1392);
}
enum class SecondIf { Opposite, OtherWord, SamePolarity, OppositeSsa, OtherHandle };

// Mortal Kombat 1 pattern: `if (c) r = h; if (!c) r = h; fetch(r)`. The register
// holds unrelated data when neither branch runs, which the two predicates exclude.
bool ComplementaryIfsHandle(SecondIf mode) {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, first{pool}, merge_first{pool}, second{pool}, merge_second{pool};
    IR::IREmitter entry_ir{entry}, first_ir{first}, second_ir{second};
    const auto data = entry_ir.GetAttributeU32(IR::Attribute::Generic0X);
    const bool ssa = mode == SecondIf::OppositeSsa;
    const IR::U1 condition = ssa
        ? entry_ir.INotEqual(entry_ir.GetAttributeU32(IR::Attribute::Generic0Y), entry_ir.Imm32(0))
        : entry_ir.INotEqual(entry_ir.GetCbuf(entry_ir.Imm32(7), entry_ir.Imm32(108)), entry_ir.Imm32(~0U));
    const auto handle = [](IR::IREmitter& ir, u32 texture) {
        return ir.BitwiseOr(ir.GetCbuf(ir.Imm32(2), ir.Imm32(texture)), ir.GetCbuf(ir.Imm32(2), ir.Imm32(1384)));
    };
    const auto first_handle = handle(first_ir, 360);
    auto first_phi = merge_first.PrependNewInst(merge_first.end(), IR::Opcode::Phi, {}, static_cast<u32>(IR::Type::U32));
    first_phi->AddPhiOperand(&entry, data);
    first_phi->AddPhiOperand(&first, first_handle);
    IR::IREmitter merge_ir{merge_first};
    // Maxwell re-emits the comparison, so the second predicate is a separate instruction.
    const u32 word = mode == SecondIf::OtherWord ? 112 : 108;
    const IR::U1 again = ssa ? condition
        : merge_ir.INotEqual(merge_ir.GetCbuf(merge_ir.Imm32(7), merge_ir.Imm32(word)), merge_ir.Imm32(~0U));
    const IR::U1 second_condition = mode == SecondIf::SamePolarity ? again : merge_ir.LogicalNot(again);
    const auto second_handle = handle(second_ir, mode == SecondIf::OtherHandle ? 368 : 360);
    auto second_phi = merge_second.PrependNewInst(merge_second.end(), IR::Opcode::Phi, {}, static_cast<u32>(IR::Type::U32));
    second_phi->AddPhiOperand(&merge_first, IR::Value{&*first_phi});
    second_phi->AddPhiOperand(&second, second_handle);
    IR::IREmitter fetch_ir{merge_second};
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Color2D);
    const auto fetch = fetch_ir.ImageFetch(IR::U32{IR::Value{&*second_phi}},
        fetch_ir.CompositeConstruct(fetch_ir.Imm32(0), fetch_ir.Imm32(0)), {}, fetch_ir.Imm32(0), {}, flags);
    IR::Program program{};
    program.stage = Stage::Fragment;
    program.blocks = {&entry, &first, &merge_first, &second, &merge_second};
    program.post_order_blocks = {&merge_second, &second, &merge_first, &first, &entry};
    using Node = IR::AbstractSyntaxNode;
    program.syntax_list = {
        {.data = {.block = &entry}, .type = Node::Type::Block},
        {.data = {.if_node = {condition, &first, &merge_first}}, .type = Node::Type::If},
        {.data = {.block = &first}, .type = Node::Type::Block},
        {.data = {.end_if = {&merge_first}}, .type = Node::Type::EndIf},
        {.data = {.block = &merge_first}, .type = Node::Type::Block},
        {.data = {.if_node = {second_condition, &second, &merge_second}}, .type = Node::Type::If},
        {.data = {.block = &second}, .type = Node::Type::Block},
        {.data = {.end_if = {&merge_second}}, .type = Node::Type::EndIf},
        {.data = {.block = &merge_second}, .type = Node::Type::Block},
    };
    FetchEnvironment env;
    env.has_pool = false;
    HostTranslateInfo host{};
    host.support_separate_sampler = true;
    host.max_bindless_descriptors_per_stage = 32;
    try {
        Optimization::TexturePass(env, program, host);
    } catch (const NotImplementedException&) {
        return false;
    }
    assert(fetch.Inst()->GetOpcode() == IR::Opcode::ImageFetch);
    assert(program.info.texture_descriptors.size() == 1);
    const auto& desc = program.info.texture_descriptors.front();
    assert(!desc.is_direct && desc.count == 1 && desc.sources.empty());
    assert(desc.cbuf_index == 2 && desc.cbuf_offset == 360);
    assert(desc.has_secondary && desc.secondary_cbuf_index == 2 && desc.secondary_cbuf_offset == 1384);
    return true;
}

// Mortal Kombat 1 compute pattern: `r = a; if (c) r = r | s; fetch(r | s)`. Both paths
// give `a | s`. A branch that ORs another word gives two handles and must stay unresolved.
bool RepeatedOrHandle(u32 branch_word) {
    using namespace Shader;
    ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, branch{pool}, merge{pool};
    IR::IREmitter entry_ir{entry}, branch_ir{branch};
    const auto texture = entry_ir.GetCbuf(entry_ir.Imm32(2), entry_ir.Imm32(1384));
    const IR::U1 condition =
        entry_ir.INotEqual(entry_ir.GetAttributeU32(IR::Attribute::Generic0X), entry_ir.Imm32(0));
    const auto merged = branch_ir.BitwiseOr(texture, branch_ir.GetCbuf(branch_ir.Imm32(2), branch_ir.Imm32(branch_word)));
    auto phi = merge.PrependNewInst(merge.end(), IR::Opcode::Phi, {}, static_cast<u32>(IR::Type::U32));
    phi->AddPhiOperand(&entry, texture);
    phi->AddPhiOperand(&branch, merged);
    IR::IREmitter fetch_ir{merge};
    const auto handle = fetch_ir.BitwiseOr(IR::U32{IR::Value{&*phi}}, fetch_ir.GetCbuf(fetch_ir.Imm32(2), fetch_ir.Imm32(424)));
    IR::TextureInstInfo flags{};
    flags.type.Assign(TextureType::Color2D);
    const auto fetch = fetch_ir.ImageFetch(handle, fetch_ir.CompositeConstruct(fetch_ir.Imm32(0), fetch_ir.Imm32(0)),
                                           {}, fetch_ir.Imm32(0), {}, flags);
    IR::Program program{};
    program.stage = Stage::Compute;
    program.blocks = {&entry, &branch, &merge};
    program.post_order_blocks = {&merge, &branch, &entry};
    using Node = IR::AbstractSyntaxNode;
    program.syntax_list = {
        {.data = {.block = &entry}, .type = Node::Type::Block},
        {.data = {.if_node = {condition, &branch, &merge}}, .type = Node::Type::If},
        {.data = {.block = &branch}, .type = Node::Type::Block},
        {.data = {.end_if = {&merge}}, .type = Node::Type::EndIf},
        {.data = {.block = &merge}, .type = Node::Type::Block},
    };
    FetchEnvironment env;
    env.has_pool = false;
    HostTranslateInfo host{};
    host.support_separate_sampler = true;
    host.max_bindless_descriptors_per_stage = 32;
    try {
        Optimization::TexturePass(env, program, host);
    } catch (const NotImplementedException&) {
        return false;
    }
    assert(fetch.Inst()->GetOpcode() == IR::Opcode::ImageFetch);
    assert(program.info.texture_descriptors.size() == 1);
    const auto& desc = program.info.texture_descriptors.front();
    assert(!desc.is_direct && desc.count == 1 && desc.sources.empty());
    assert(desc.cbuf_index == 2 && desc.cbuf_offset == 424);
    assert(desc.has_secondary && desc.secondary_cbuf_index == 2 && desc.secondary_cbuf_offset == 1384);
    return true;
}
} // namespace

void TestRuntimeTextureFetch() {
    ConstantBitfieldFlags();
    FlaggedPredicateHandle(false);
    FlaggedPredicateHandle(true);
    FlaggedPredicateHandle(false, true);
    FlaggedPredicateHandle(true, true);
    PredicatePhiFacts();
    RuntimeFetch(true, 32, false); // Reachable unknown handles must not allocate the full TIC table.
    RuntimeFetch(false, 32, false);
    RuntimeFetch(true, 8, false);
    ImpossibleUniformFetch(true);
    ImpossibleUniformFetch(false);
    ImpossibleUniformFetch(true, false); // SSA/loop-varying predicates stay conservative.
    ContradictoryOuterPredicateHandle();
    assert(ComplementaryIfsHandle(SecondIf::Opposite));
    assert(ComplementaryIfsHandle(SecondIf::OppositeSsa));
    // The data path stays reachable: these must keep failing instead of guessing.
    assert(!ComplementaryIfsHandle(SecondIf::OtherWord));
    assert(!ComplementaryIfsHandle(SecondIf::SamePolarity));
    // Two proven handles on the two paths are not one static descriptor.
    assert(!ComplementaryIfsHandle(SecondIf::OtherHandle));
    assert(RepeatedOrHandle(424));
    assert(!RepeatedOrHandle(432));
}
