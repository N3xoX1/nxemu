// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <vector>

#include "nxemu-video/video_settings.h"
#include "yuzu_shader_recompiler/backend/bindings.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/object_pool.h"

VideoSettings videoSettings{};
namespace IR = Shader::IR;

namespace {
struct Options {
    bool workaround{true};
    bool unorm8{true};
    bool dither{};
    bool dual_source{};
    bool writes_secondary{true};
    bool writes_alpha{true};
    bool declare_unused{};
    std::optional<u32> component_mask;
    Shader::AttributeType type{Shader::AttributeType::Float};
    std::optional<Shader::CompareFunction> comparison;
    float alpha{};
};

std::vector<u32> Compile(const Options& options) {
    Shader::ObjectPool<IR::Inst> inst_pool{64};
    Shader::ObjectPool<IR::Block> block_pool{4};
    auto* block = block_pool.Create(inst_pool);
    IR::IREmitter ir{*block};
    const u32 mask = options.component_mask.value_or(options.writes_alpha ? 15U : 7U);
    ir.Prologue();
    for (u32 target = 0; target < (options.writes_secondary ? 2U : 1U); ++target) {
        for (u32 component = 0; component < 4; ++component) {
            if ((mask & (1U << component)) == 0) continue;
            ir.SetFragColor(target, component,
                            ir.Imm32(component == 3 ? options.alpha : 0.5f));
        }
    }
    ir.Epilogue();
    IR::Program program{};
    program.stage = Shader::Stage::Fragment;
    program.blocks.push_back(block);
    program.post_order_blocks.push_back(block);
    for (u32 target = 0; target < (options.writes_secondary ? 2U : 1U); ++target) {
        program.info.stores_frag_color[target] = true;
        program.info.stores_frag_alpha[target] = (mask & 8U) != 0;
    }
    program.syntax_list.push_back({.data{.block = block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.has_unorm8_round_half_up = options.workaround;
    profile.need_declared_frag_colors = options.declare_unused;
    Shader::RuntimeInfo runtime{};
    runtime.alpha_test_func = options.comparison;
    runtime.alpha_to_coverage_dither = options.dither;
    runtime.dual_source_blend = options.dual_source;
    runtime.color_output_types[0] = options.type;
    runtime.color_output_types[1] = options.dual_source ? Shader::AttributeType::UnsignedInt
                                                     : Shader::AttributeType::Float;
    runtime.color_output_unorm8[0] = options.unorm8;
    runtime.color_output_unorm8[1] = !options.dual_source && options.unorm8;
    Shader::Backend::Bindings bindings{};
    return Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
}

struct Observations {
    std::vector<float> compared_alpha;
    std::vector<float> output_alpha;
    unsigned corrections{};
    unsigned corrected_outputs{};
    bool killed{};
    unsigned scalar_loads{}, vector_loads{};
    std::array<unsigned, 4> component_stores{};
};

// Follow constants, output memory and arithmetic in the emitted shader. Observe comparison
// operands before control flow: alpha testing and coverage must see the guest's original value.
Observations Observe(const std::vector<u32>& words, bool execute_branches = false) {
    struct Value {
        std::array<std::optional<float>, 4> components;
    };
    struct Pointer { u32 base{}; std::optional<u32> component; };
    std::vector<Value> values(words[3]);
    std::vector<Pointer> pointers(words[3]);
    std::vector<u32> literals(words[3]);
    std::vector<bool> corrected(words[3]);
    std::vector<bool> conditions(words[3]);
    std::vector<size_t> labels(words[3]);
    for (size_t i = 5; i < words.size(); i += words[i] >> 16) {
        if (static_cast<spv::Op>(words[i] & 0xffff) == spv::Op::OpLabel) {
            labels[words[i + 1]] = i;
        }
    }
    Observations result;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16;
        const auto op = static_cast<spv::Op>(words[i] & 0xffff);
        const auto* w = words.data() + i;
        switch (op) {
        case spv::Op::OpConstant:
            if (count == 4) {
                literals[w[2]] = w[3];
                values[w[2]].components[0] = std::bit_cast<float>(w[3]);
            }
            break;
        case spv::Op::OpConstantComposite:
            for (u32 component = 0; component < count - 3 && component < 4; ++component) {
                values[w[2]].components[component] = values[w[3 + component]].components[0];
            }
            break;
        case spv::Op::OpVariable:
            pointers[w[2]].base = w[2];
            break;
        case spv::Op::OpAccessChain:
            pointers[w[2]] = {pointers[w[3]].base, literals[w[4]]};
            break;
        case spv::Op::OpStore: {
            const auto pointer = pointers[w[1]];
            if (pointer.component) {
                ++result.component_stores[*pointer.component];
                values[pointer.base].components[*pointer.component] = values[w[2]].components[0];
            } else {
                values[pointer.base] = values[w[2]];
            }
            corrected[pointer.base] = corrected[pointer.base] || corrected[w[2]];
            break;
        }
        case spv::Op::OpLoad: {
            const auto pointer = pointers[w[3]];
            if (pointer.component) ++result.scalar_loads;
            else ++result.vector_loads;
            if (pointer.component) {
                values[w[2]].components[0] = values[pointer.base].components[*pointer.component];
            } else {
                values[w[2]] = values[pointer.base];
            }
            break;
        }
        case spv::Op::OpCompositeExtract:
            values[w[2]].components[0] = values[w[3]].components[w[4]];
            break;
        case spv::Op::OpFSub:
            ++result.corrections;
            corrected[w[2]] = true;
            for (size_t component = 0; component < 4; ++component) {
                const auto a = values[w[3]].components[component];
                const auto b = values[w[4]].components[component];
                if (a && b) {
                    values[w[2]].components[component] = *a - *b;
                }
            }
            break;
        case spv::Op::OpFOrdLessThan:
        case spv::Op::OpFOrdEqual:
        case spv::Op::OpFOrdLessThanEqual:
        case spv::Op::OpFOrdGreaterThan:
        case spv::Op::OpFOrdNotEqual:
        case spv::Op::OpFOrdGreaterThanEqual:
        {
            const float lhs = values[w[3]].components[0].value_or(NAN);
            const float rhs = values[w[4]].components[0].value_or(NAN);
            result.compared_alpha.push_back(lhs);
            const bool ordered = !std::isnan(lhs) && !std::isnan(rhs);
            switch (op) {
            case spv::Op::OpFOrdLessThan: conditions[w[2]] = ordered && lhs < rhs; break;
            case spv::Op::OpFOrdEqual: conditions[w[2]] = ordered && lhs == rhs; break;
            case spv::Op::OpFOrdLessThanEqual: conditions[w[2]] = ordered && lhs <= rhs; break;
            case spv::Op::OpFOrdGreaterThan: conditions[w[2]] = ordered && lhs > rhs; break;
            case spv::Op::OpFOrdNotEqual: conditions[w[2]] = ordered && lhs != rhs; break;
            case spv::Op::OpFOrdGreaterThanEqual: conditions[w[2]] = ordered && lhs >= rhs; break;
            default: break;
            }
            break;
        }
        case spv::Op::OpBranchConditional:
            if (execute_branches) {
                i = labels[conditions[w[1]] ? w[2] : w[3]];
                continue;
            }
            break;
        case spv::Op::OpBranch:
            if (execute_branches) {
                i = labels[w[1]];
                continue;
            }
            break;
        case spv::Op::OpKill:
            if (execute_branches) {
                result.killed = true;
                return result;
            }
            break;
        default:
            break;
        }
        i += count;
    }
    for (size_t id = 0; id < pointers.size(); ++id) {
        // Only output variables have a self-referencing pointer and corrected stores.
        if (pointers[id].base == id && corrected[id]) {
            ++result.corrected_outputs;
            result.output_alpha.push_back(values[id].components[3].value_or(NAN));
        }
    }
    return result;
}
} // namespace

int main(int argc, char** argv) {
    unsigned failures{};
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
    };
    for (const auto comparison : {Shader::CompareFunction::Less, Shader::CompareFunction::Equal,
             Shader::CompareFunction::LessThanEqual, Shader::CompareFunction::Greater,
             Shader::CompareFunction::NotEqual, Shader::CompareFunction::GreaterThanEqual}) {
        for (float alpha : {0.0f, 1.0f, 0.5f, 0.5f / 255.0f}) {
            const auto observed = Observe(Compile({.comparison = comparison, .alpha = alpha}));
            check(observed.compared_alpha.size() == 1 && observed.compared_alpha[0] == alpha,
                  "alpha comparisons use the unmodified guest value, including zero != zero");
            check(observed.corrected_outputs == 2, "both UNORM8 attachments retain rounding correction");
            const float rounded = alpha - 1.0f / (255.0f * 512.0f);
            check(observed.output_alpha == std::vector<float>{rounded, rounded},
                  "surviving outputs still receive the UNORM8 rounding correction");
        }
    }
    const auto coverage = Observe(Compile({.dither = true,
        .comparison = Shader::CompareFunction::NotEqual, .alpha = 0.5f}));
    check(coverage.compared_alpha == std::vector<float>{0.5f, 0.5f},
          "alpha test and alpha-to-coverage both precede output rounding");
    check(Observe(Compile({.workaround = false})).corrections == 0,
          "other drivers emit no rounding correction");
    check(Observe(Compile({.unorm8 = false})).corrections == 0,
          "non-UNORM8 attachments remain untouched");
    check(Observe(Compile({.writes_secondary = false, .type = Shader::AttributeType::UnsignedInt})).corrections == 0 &&
          Observe(Compile({.writes_secondary = false, .type = Shader::AttributeType::SignedInt})).corrections == 0,
          "integer attachments remain untouched");
    check(Observe(Compile({.dual_source = true})).corrected_outputs == 2,
          "dual-source outputs use attachment zero's format");
    check(Observe(Compile({.dual_source = true, .writes_secondary = false})).corrected_outputs == 1,
          "unwritten dual-source output keeps its zero initialization");
    check(Observe(Compile({.declare_unused = true})).corrected_outputs == 2,
          "unused declared attachments are not read or corrected");
    check(Observe(Compile({.dither = true, .writes_alpha = false})).compared_alpha.empty(),
          "RGB-only writes do not enable alpha-to-coverage");
    for (u32 mask : {1U, 2U, 4U, 7U, 8U, 9U, 15U}) {
        const auto partial = Observe(Compile({.component_mask = mask}));
        check(partial.vector_loads == 0, "partial writes add no full RGBA reads");
        check(partial.scalar_loads == ((mask & 8U) ? 1U : 0U),
              "only a written RT0 alpha is loaded for deferred correction");
        check(partial.corrections == 2U * std::popcount(mask),
              "only written components receive a rounding correction");
        for (u32 component = 0; component < 4; ++component) {
            const unsigned expected = (mask & (1U << component))
                ? (component == 3 ? 3U : 2U) : 0U;
            check(partial.component_stores[component] == expected,
                  "unwritten components are never stored by rounding correction");
        }
    }
    // Execute the actual emitted branches for the original transparent-overlay failure.
    for (float alpha : {0.0f, 0.5f}) {
        check(Observe(Compile({.comparison = Shader::CompareFunction::NotEqual,
                  .alpha = alpha}), true).killed == (alpha == 0.0f),
              "NotEqual-zero kills transparent fragments and retains visible ones");
        check(Observe(Compile({.comparison = Shader::CompareFunction::Equal,
                  .alpha = alpha}), true).killed == (alpha != 0.0f),
              "Equal-zero takes the opposite discard branch");
    }
    if (argc > 1) {
        const auto words = Compile({.dither = true, .dual_source = true,
            .comparison = Shader::CompareFunction::NotEqual});
        if (FILE* file = std::fopen(argv[1], "wb")) {
            std::fwrite(words.data(), sizeof(u32), words.size(), file);
            std::fclose(file);
        }
    }
    std::printf("Fragment color rounding: %u failures\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
