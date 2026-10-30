// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>
#include "nxemu-video/video_settings.h"
#include "yuzu_shader_recompiler/alpha_to_coverage_dither.h"
#include "yuzu_shader_recompiler/backend/bindings.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/object_pool.h"
#include "yuzu_shader_recompiler/profile.h"
#include "yuzu_shader_recompiler/runtime_info.h"

VideoSettings videoSettings{};

namespace IR = Shader::IR;

namespace {
unsigned Kept(float alpha, u32 size = 128, u32 x0 = 63, u32 y0 = 119) {
    unsigned kept{};
    for (u32 y = y0; y < y0 + size; ++y) {
        for (u32 x = x0; x < x0 + size; ++x) {
            kept += Shader::AlphaToCoverageDitherKeeps(alpha, x, y) ? 1 : 0;
        }
    }
    return kept;
}

struct Module {
    std::vector<u32> words;
    bool kill{};
    bool frag_coord{};
    bool ign{};
    unsigned multiplies{}, fractions{}, adds{}, contractions{}, loops{}, texture_reads{};
};

// Compile a fragment shader that writes the alpha of render target 0 through the production
// SPIR-V backend.
Module Compile(bool dither, bool writes_color, bool writes_alpha = true,
               Shader::AttributeType output = Shader::AttributeType::Float) {
    Shader::ObjectPool<IR::Inst> inst_pool{64};
    Shader::ObjectPool<IR::Block> block_pool{4};
    IR::Block* const block = block_pool.Create(inst_pool);
    IR::IREmitter ir{*block};
    ir.Prologue();
    if (writes_color) {
        for (u32 component = 0; component < (writes_alpha ? 4U : 3U); ++component) {
            ir.SetFragColor(0, component, ir.Imm32(0.5f));
        }
    }
    ir.Epilogue();
    IR::Program program{};
    program.stage = Shader::Stage::Fragment;
    program.blocks.push_back(block);
    program.post_order_blocks.push_back(block);
    program.info.stores_frag_color[0] = writes_color;
    program.info.stores_frag_alpha[0] = writes_color && writes_alpha;
    program.syntax_list.push_back({.data{.block = block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    Shader::RuntimeInfo runtime{};
    runtime.alpha_to_coverage_dither = dither;
    runtime.color_output_types[0] = output;
    Shader::Backend::Bindings bindings{};
    Module module;
    module.words = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    const auto& w = module.words;
    for (size_t i = 5; i < w.size();) {
        const u32 count = w[i] >> 16;
        if (count == 0) {
            break;
        }
        const auto op = static_cast<spv::Op>(w[i] & 0xffff);
        module.kill |= op == spv::Op::OpKill;
        module.ign |= op == spv::Op::OpConstant && count == 4 &&
                      w[i + 3] == std::bit_cast<u32>(Shader::IGN_MULTIPLIER);
        module.multiplies += op == spv::Op::OpFMul;
        module.adds += op == spv::Op::OpFAdd;
        // GLSL.std.450 Fract is instruction 10.
        module.fractions += op == spv::Op::OpExtInst && count >= 6 && w[i + 4] == 10;
        module.contractions += op == spv::Op::OpDecorate && count == 3 &&
                               w[i + 2] == static_cast<u32>(spv::Decoration::NoContraction);
        module.loops += op == spv::Op::OpLoopMerge;
        module.texture_reads += op == spv::Op::OpImageSampleImplicitLod ||
                                op == spv::Op::OpImageSampleExplicitLod || op == spv::Op::OpImageFetch;
        module.frag_coord |= op == spv::Op::OpDecorate && count == 4 &&
                             w[i + 2] == static_cast<u32>(spv::Decoration::BuiltIn) &&
                             w[i + 3] == static_cast<u32>(spv::BuiltIn::FragCoord);
        i += count;
    }
    return module;
}
} // namespace

int main(int argc, char** argv) {
    unsigned failures{};
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
    };

    check(Kept(0.0f) == 0 && Kept(1.0f) == 128 * 128, "alpha endpoints stay fully transparent/opaque");
    check(Kept(-1.0f) == 0 && Kept(2.0f) == 128 * 128, "out-of-range alpha saturates coverage");
    check(Kept(std::numeric_limits<float>::quiet_NaN()) == 0, "NaN alpha is transparent");
    check(Kept(-std::numeric_limits<float>::infinity()) == 0 &&
          Kept(std::numeric_limits<float>::infinity()) == 128 * 128, "infinite alpha endpoints");

    for (u32 scale : {1U, 2U, 3U, 4U, 6U}) {
        const u32 size = 128 * scale;
        for (float alpha : {.01f, .1f, .125f, .3f, .4f, .5f, .6f, .75f, .9f, .99f}) {
            const float coverage = float(Kept(alpha, size)) / float(size * size);
            check(std::abs(coverage - alpha) < .005f,
                  "coverage follows continuous alpha at every resolution scale");
        }
    }
    check(Kept(.41f) > Kept(.40f) + 100,
          "small alpha changes produce a fade instead of eight quantized levels");

    bool nested = true, repeated = true;
    unsigned changed_after_two{}, mixed_blocks{};
    for (u32 y = 0; y < 128; ++y) {
        for (u32 x = 0; x < 128; ++x) {
            const bool keep = Shader::AlphaToCoverageDitherKeeps(.4f, x, y);
            nested &= !keep || Shader::AlphaToCoverageDitherKeeps(.41f, x, y);
            repeated &= keep == Shader::AlphaToCoverageDitherKeeps(.4f, x, y);
            changed_after_two += keep != Shader::AlphaToCoverageDitherKeeps(.4f, x + 2, y);
            if ((x % 4 == 0) && (y % 4 == 0)) {
                const unsigned count = Kept(.5f, 4, x, y);
                mixed_blocks += count > 0 && count < 16;
            }
        }
    }
    check(nested && repeated, "coverage is spatially stable and monotonic during a fade");
    check(changed_after_two > 2048, "no repeating 2x2 checkerboard");
    check(mixed_blocks == 32 * 32, "4x4 host blocks contain fine mixed coverage");

    const Module off = Compile(false, true);
    const Module on = Compile(true, true);
    const Module no_color = Compile(true, false);
    check(!off.kill && !off.frag_coord, "without dither the shader has no discard");
    check(on.kill && on.frag_coord && on.ign, "IGN shader reads host FragCoord and discards");
    check(on.multiplies == 3 && on.adds == 1 && on.fractions == 2 &&
          on.loops == 0 && on.texture_reads == 0, "IGN uses bounded arithmetic, no loops or texture reads");
    check(on.contractions == 4, "IGN arithmetic prevents compiler-dependent contraction");
    check(!no_color.kill, "a shader that does not write the color is left alone");
    check(!Compile(true, true, false).kill, "a shader that does not write alpha is left alone");
    check(!Compile(true, true, true, Shader::AttributeType::UnsignedInt).kill &&
          !Compile(true, true, true, Shader::AttributeType::SignedInt).kill,
          "integer color attachments remain untouched");
    for (u32 scale : {1U, 2U, 4U}) {
        videoSettings.resolution_info.up_scale = scale;
        videoSettings.resolution_info.up_factor = float(scale);
        videoSettings.resolution_info.down_factor = 1.0f / float(scale);
        videoSettings.resolution_info.active = scale != 1;
        check(Compile(true, true).words == on.words,
              "IGN shader does not change with guest resolution scaling");
    }

    if (argc > 1) {
        // Write the module for spirv-val
        if (FILE* file = std::fopen(argv[1], "wb")) {
            std::fwrite(on.words.data(), sizeof(u32), on.words.size(), file);
            std::fclose(file);
        }
    }
    std::printf("Alpha-to-coverage dither: %u failures\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
