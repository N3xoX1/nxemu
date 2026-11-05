// SPDX-License-Identifier: GPL-2.0-or-later
#include "nxemu-video/video_settings.h"
#include "yuzu_shader_recompiler/frontend/ir/ir_emitter.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_shader_recompiler/ir_opt/reconstruction_pass.h"
#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

VideoSettings videoSettings{};
namespace IR = Shader::IR;

namespace
{
constexpr std::array<float, 6> constants{0.2f, 0.799375f, 1.0f / 1600,
                                         1.0f / 900, 1.0f / 1280, 1.0f / 720};
std::map<std::pair<u32, u32>, float> cbuf;
bool sampled_images_scaled{true};
float Evaluate(IR::Value value, float x, float y, float down)
{
    value = value.Resolve();
    if (value.IsImmediate())
    {
        if (value.Type() == IR::Type::F32)
            return value.F32();
        if (value.Type() == IR::Type::U1)
            return float(value.U1());
        return float(value.U32());
    }
    const auto & inst = *value.Inst();
    const auto arg = [&](size_t n) { return Evaluate(inst.Arg(n), x, y, down); };
    switch (inst.GetOpcode())
    {
    case IR::Opcode::GetAttribute:
        return inst.Arg(0).Attribute() == IR::Attribute::PositionX ? x : y;
    case IR::Opcode::ResolutionDownFactor:
        return down;
    case IR::Opcode::GetCbufF32:
        return cbuf.at({inst.Arg(0).U32(), inst.Arg(1).U32()});
    case IR::Opcode::FPMul32:
        return arg(0) * arg(1);
    case IR::Opcode::FPAdd32:
        return arg(0) + arg(1);
    case IR::Opcode::FPNeg32:
        return -arg(0);
    case IR::Opcode::FPFma32:
        return std::fma(arg(0), arg(1), arg(2));
    case IR::Opcode::FPTrunc32:
        return std::trunc(arg(0));
    case IR::Opcode::ConvertS32F32:
        return float(static_cast<s32>(arg(0)));
    case IR::Opcode::ConvertF32S32:
        return arg(0);
    case IR::Opcode::ConvertF32U32:
        return arg(0);
    case IR::Opcode::BitwiseAnd32:
        return float(u32(arg(0)) & u32(arg(1)));
    case IR::Opcode::IsTextureScaled:
        return sampled_images_scaled || inst.Arg(0).U32() != 9;
    case IR::Opcode::LogicalAnd:
        return arg(0) != 0 && arg(1) != 0;
    case IR::Opcode::SelectF32:
        return arg(0) != 0 ? arg(1) : arg(2);
    default:
        throw std::runtime_error("Unsupported evaluation opcode");
    }
}
void Near(float actual, float expected)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > 2e-7f)
    {
        throw std::runtime_error("Reconstruction coordinate/parity regression: " +
                                 std::to_string(actual) + " expected " +
                                 std::to_string(expected));
    }
}
void Check(unsigned scenario, u32 bank_id, u32 base, float global_scale,
           float draw_scale, bool disabled)
{
    videoSettings.renderer_backend =
        scenario == 8 ? RendererBackend::OpenGL : RendererBackend::Vulkan;
    sampled_images_scaled = scenario != 9;
    videoSettings.resolution_info.up_factor = global_scale;
    videoSettings.resolution_info.down_factor = 1.0f / global_scale;
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    IR::IREmitter ir(block);
    ir.Prologue();
    const IR::F32 x = ir.GetAttribute(IR::Attribute::PositionX);
    const IR::F32 y = ir.GetAttribute(IR::Attribute::PositionY);
    const IR::U32 integer_x{ir.ConvertFToS(32, ir.FPTrunc(x))};
    const IR::U32 half_x{
        ir.ConvertFToS(32, ir.FPTrunc(ir.FPMul(x, ir.Imm32(0.5f))))};
    const IR::F32 pair_center{
        ir.FPAdd(ir.FPMul(ir.ConvertSToF(32, 32, half_x), ir.Imm32(2.0f)),
                 ir.Imm32(1.0f))};
    cbuf.clear();
    for (u32 i = 0; i < constants.size(); ++i)
        cbuf[{bank_id, base + i * 4}] = constants[i];
    const IR::U32 bank = ir.Imm32(bank_id);
    const auto cb = [&](u32 offset) {
        return ir.GetFloatCbuf(bank, ir.Imm32(base + offset));
    };
    const IR::F32 u{
        scenario == 7 ? ir.FPFma(ir.FPMul(ir.ConvertSToF(32, 32, half_x), cb(8)),
                                 ir.Imm32(2.0f), cb(8))
                      : ir.FPMul(pair_center, cb(8))};
    const IR::F32 v{ir.FPFma(y, cb(12), cb(0))};
    const IR::F32 limit = cb(4);
    const IR::U32 parity{ir.BitwiseAnd(integer_x, ir.Imm32(1U))};
    const auto condition = ir.IEqual(parity, ir.Imm32(0U));
    ir.ConditionRef(condition);
    const auto sample = [&](IR::F32 sx, IR::F32 sy, u32 descriptor) -> IR::F32 {
        IR::TextureInstInfo info{};
        info.type.Assign(Shader::TextureType::Color2D);
        info.descriptor_index.Assign(descriptor);
        auto sampled = block.PrependNewInst(
            block.end(), IR::Opcode::ImageSampleImplicitLod,
            {IR::Value{}, ir.CompositeConstruct(sx, sy), IR::Value{}, IR::Value{}},
            info.raw);
        return IR::F32{ir.CompositeExtract(IR::Value{&*sampled}, 0)};
    };
    const auto a = sample(u, v, 5);
    const auto b = sample(IR::F32{ir.FPMul(pair_center, cb(16))},
                          IR::F32{ir.FPMul(y, cb(20))}, scenario == 3 ? 5 : 9);
    ir.SetFragColor(0, 0, a);
    ir.SetFragColor(scenario == 4 ? 0 : 1, 1, b);
    ir.SetFragColor(0, 2, limit);
    if (scenario ==
        1) // Deliberate parity color pattern, not a reconstruction predicate.
        ir.SetFragColor(0, 3, IR::F32{ir.ConvertUToF(32, 32, parity)});
    if (scenario ==
        2) // A coefficient also serving as a color weight is ambiguous.
        ir.SetFragColor(0, 3, cb(8));
    if (scenario == 5) // Material/geometry input: keep the generic rescaler.
        ir.SetFragColor(0, 3, ir.GetAttribute(IR::Attribute::Generic0X));
    if (scenario ==
        6) // A boolean parity result used as a color must also be rejected.
        ir.SetFragColor(
            0, 3, IR::F32{ir.Select(condition, ir.Imm32(1.0f), ir.Imm32(0.0f))});
    ir.Epilogue();
    const auto inferred = Shader::Optimization::FindReconstructionSteps(program);
    const bool recognized =
        scenario == 0 || scenario == 7 || scenario == 8 || scenario == 9;
    if (inferred.loads.empty() == recognized)
        throw std::runtime_error("Recognition regression");
    if (recognized)
    {
        std::set<std::pair<u32, u32>> keys;
        for (auto * load : inferred.loads)
            keys.emplace(load->Arg(0).U32(), load->Arg(1).U32());
        const std::set<std::pair<u32, u32>> expected{{bank_id, base + 8},
                                                     {bank_id, base + 12},
                                                     {bank_id, base + 16},
                                                     {bank_id, base + 20}};
        if (keys != expected)
            throw std::runtime_error("Dimension inference regression");
    }
    Shader::Optimization::RescalingPass(program);
    Shader::Optimization::DeadCodeEliminationPass(program);
    Shader::Optimization::VerificationPass(program);
    const bool corrected =
        !disabled && global_scale > 1 && (scenario == 0 || scenario == 7);
    // Independent reference: centers of pairs on the actual host pixel grid,
    // with normalized bounds. Probe the whole useful width, including both edges.
    const unsigned width = static_cast<unsigned>(1280 * draw_scale);
    for (unsigned column = 0; column < width; ++column)
    {
        const float px = float(column) + 0.5f;
        const float py = float(column % 720) + 0.5f;
        const float logical_x = corrected ? px : px / draw_scale;
        const float divisor = corrected ? draw_scale : 1.0f;
        const float expected_u =
            float(2 * unsigned(logical_x / 2) + 1) / (1600 * divisor);
        Near(Evaluate(u, px, py, 1 / draw_scale), expected_u);
        Near(Evaluate(v, px, py, 1 / draw_scale), py / (900 * draw_scale) + 0.2f);
        Near(Evaluate(limit, px, py, 1 / draw_scale), constants[1]);
        Near(Evaluate(parity, px, py, 1 / draw_scale),
             float(unsigned(logical_x) & 1));
    }
}
} // namespace

int main(int argc, char ** argv)
{
    try
    {
        const bool disabled = argc > 1 && std::string_view(argv[1]) == "--disabled";
        for (unsigned scenario = 0; scenario <= 9; ++scenario)
        {
            for (auto layout :
                 {std::pair{3U, 0U}, std::pair{7U, 128U}, std::pair{1U, 64U}})
            {
                for (float scale : {0.5f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f})
                {
                    Check(scenario, layout.first, layout.second, scale, scale, disabled);
                    if (scale > 1)
                        Check(scenario, layout.first, layout.second, scale, 1.0f, disabled);
                }
            }
        }
        std::cout << "Reconstruction IR: 2 algebraic forms, 3 cbuf layouts, "
                     "0.5x-6x, native/GL, 6 negative cases OK\n";
    }
    catch (const std::exception & exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
