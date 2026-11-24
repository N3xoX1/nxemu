// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_guest_state.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include "yuzu_common/bit_cast.h"
#include "yuzu_shader_recompiler/shader_info.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/texture_cache/format_lookup_table.h"

namespace VideoCommon::ShaderProfile::GuestState
{
namespace
{
using Vulkan::FixedPipelineState;
using Vulkan::GuestPipelineState;
using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using EngineHint = Tegra::Engines::Maxwell3D::EngineHint;
static_assert(sizeof(Maxwell) == RegisterCount * sizeof(u32));

struct ZeroedRegisters
{
    std::unique_ptr<Maxwell> regs = std::make_unique<Maxwell>();
    ZeroedRegisters() { std::memset(static_cast<void *>(regs.get()), 0, sizeof(Maxwell)); }
};
u32 & Word(Maxwell & regs, size_t index) { return regs.reg_array[index]; }

// The registers RefreshFromRegisters reads, written so that they give `state` back.
// Each packed field takes one register value that packs to it; fields the
// packing derives from several registers take the simplest such combination.
bool Write(const GuestPipelineState & state, Maxwell & regs, u32 & topology, u32 & hint)
{
    const auto & s = state.complete;
    using Fixed = FixedPipelineState;
    regs.transform_feedback_enabled = s.xfb_enabled;
    regs.depth_mode = s.ndc_minus_one_to_one ? Maxwell::DepthMode::MinusOneToOne : Maxwell::DepthMode::ZeroToOne;
    regs.polygon_mode_front = Fixed::UnpackPolygonMode(s.polygon_mode);
    regs.tessellation.params.domain_type.Assign(static_cast<Maxwell::Tessellation::DomainType>(s.tessellation_primitive.Value()));
    regs.tessellation.params.spacing.Assign(static_cast<Maxwell::Tessellation::Spacing>(s.tessellation_spacing.Value()));
    regs.tessellation.params.output_primitives.Assign(s.tessellation_clockwise ? Maxwell::Tessellation::OutputPrimitives::Triangles_CW
                                                                               : Maxwell::Tessellation::OutputPrimitives::Points);
    regs.patch_vertices = s.patch_control_points_minus_one + 1;
    topology = static_cast<u32>(s.topology.Value());
    const auto msaa = s.msaa_mode.Value();
    if (s.tir_resolve_color)
    {
        // Only depth takes the raster sample count: target-independent raster with depth, single-sample colors.
        regs.tir_mode = Maxwell::TIRMode::RasterNTargetM;
        regs.anti_alias_raster = static_cast<Maxwell::AntiAliasRaster>(msaa);
        regs.anti_alias_samples_mode = Tegra::Texture::MsaaMode::Msaa1x1;
    }
    else
        regs.anti_alias_samples_mode = msaa;
    const auto func = s.alpha_test_func.Value();
    if (func != Fixed::PackComparisonOp(Maxwell::ComparisonOp::Always_GL))
    {
        regs.alpha_test_enabled = 1;
        regs.alpha_test_func = Fixed::UnpackComparisonOp(func);
    }
    regs.alpha_test_ref = Common::BitCast<f32>(s.alpha_test_ref);
    regs.mandated_early_z = s.early_z;
    regs.zeta_enable = s.depth_enabled;
    regs.zeta.format = static_cast<Tegra::DepthFormat>(s.depth_format.Value());
    regs.window_origin.mode.Assign(s.y_negate ? Maxwell::WindowOrigin::Mode::LowerLeft : Maxwell::WindowOrigin::Mode::UpperLeft);
    regs.provoking_vertex = s.provoking_vertex_last ? Maxwell::ProvokingVertex::Last : Maxwell::ProvokingVertex::First;
    regs.conservative_raster_enable = s.conservative_raster_enable;
    regs.line_anti_alias_enable = s.smooth_lines;
    regs.anti_alias_alpha_control.alpha_to_coverage.Assign(s.alpha_to_coverage_enabled);
    regs.anti_alias_alpha_control.alpha_to_one.Assign(s.alpha_to_one_enabled);
    if (s.alpha_to_coverage_dither) regs.alpha_to_coverage_dither = Maxwell::AlphaToCoverageDither::Footprint_2x2;
    hint = static_cast<u32>(s.app_stage.Value());
    for (size_t i = 0; i < regs.rt.size(); ++i)
        regs.rt[i].format = static_cast<Tegra::RenderTargetFormat>(s.color_formats[i]);
    regs.point_size = Common::BitCast<f32>(s.point_size);
    // Vertex input: attributes not read are constant ones.
    for (size_t i = 0; i < Maxwell::NumVertexAttributes; ++i)
    {
        const auto & attribute = s.attributes[i];
        auto & input = regs.vertex_attrib_format[i];
        if (!attribute.enabled)
        {
            if (attribute.raw != 0) return false;
            input.constant.Assign(1);
            continue;
        }
        input.buffer.Assign(attribute.buffer);
        input.offset.Assign(attribute.offset);
        input.type.Assign(static_cast<Maxwell::VertexAttribute::Type>(attribute.type.Value()));
        input.size.Assign(static_cast<Maxwell::VertexAttribute::Size>(attribute.size.Value()));
    }
    if (s.enabled_divisors >> Maxwell::NumVertexArrays) return false;
    for (size_t i = 0; i < Maxwell::NumVertexArrays; ++i)
    {
        auto & stream = regs.vertex_streams[i];
        stream.stride.Assign(s.vertex_strides[i]);
        stream.enable.Assign((state.enabled_vertex_streams >> i) & 1);
        if ((s.enabled_divisors >> i) & 1)
        {
            regs.vertex_stream_instances.is_instanced[i] = 1;
            stream.frequency = s.binding_divisors[i];
        }
    }
    for (size_t i = 0; i < Maxwell::NumViewports; ++i)
        regs.viewport_transform[i].swizzle.raw = s.viewport_swizzles[i];
    // Fixed-function state; the front face is never flipped and the back stencil
    // face is separate only when it differs.
    const auto & d = s.dynamic_state;
    if (d.cull_face > 2) return false;
    regs.stencil_front_op.fail = Fixed::UnpackStencilOp(d.front.action_stencil_fail);
    regs.stencil_front_op.zfail = Fixed::UnpackStencilOp(d.front.action_depth_fail);
    regs.stencil_front_op.zpass = Fixed::UnpackStencilOp(d.front.action_depth_pass);
    regs.stencil_front_op.func = Fixed::UnpackComparisonOp(d.front.test_func);
    const u32 front = d.raw2 & 0xFFF, back = (d.raw2 >> 12) & 0xFFF;
    if (front != back)
    {
        regs.stencil_two_side_enable = 1;
        regs.stencil_back_op.fail = Fixed::UnpackStencilOp(d.back.action_stencil_fail);
        regs.stencil_back_op.zfail = Fixed::UnpackStencilOp(d.back.action_depth_fail);
        regs.stencil_back_op.zpass = Fixed::UnpackStencilOp(d.back.action_depth_pass);
        regs.stencil_back_op.func = Fixed::UnpackComparisonOp(d.back.test_func);
    }
    regs.stencil_enable = d.stencil_enable;
    regs.depth_write_enabled = d.depth_write_enable;
    regs.depth_bounds_enable = d.depth_bounds_enable;
    regs.depth_test_enable = d.depth_test_enable;
    regs.gl_front_face = Fixed::UnpackFrontFace(d.front_face);
    regs.depth_test_func = Fixed::UnpackComparisonOp(d.depth_test_func);
    regs.gl_cull_face = Fixed::UnpackCullFace(d.cull_face);
    regs.gl_cull_test_enabled = d.cull_enable;
    regs.logic_op.op = Fixed::UnpackLogicOp(d.logic_op);
    regs.logic_op.enable = d.logic_op_enable;
    regs.rasterize_enable = d.rasterize_enable;
    regs.primitive_restart.enabled = d.primitive_restart_enable;
    // Depth bias follows the offset enable of the topology's class: all three agree.
    regs.polygon_offset_point_enable = d.depth_bias_enable;
    regs.polygon_offset_line_enable = d.depth_bias_enable;
    regs.polygon_offset_fill_enable = d.depth_bias_enable;
    regs.viewport_clip_control.geometry_clip.Assign(d.depth_clamp_disabled ? Maxwell::ViewportClipControl::GeometryClip::Passthrough
                                                                           : Maxwell::ViewportClipControl::GeometryClip::WZero);
    // Blending: per-target state, each target with its own write mask.
    regs.blend_per_target_enabled = 1;
    for (size_t i = 0; i < Maxwell::NumRenderTargets; ++i)
    {
        const auto & attachment = s.attachments[i];
        auto & mask = regs.color_mask[i];
        mask.R.Assign(attachment.mask_r);
        mask.G.Assign(attachment.mask_g);
        mask.B.Assign(attachment.mask_b);
        mask.A.Assign(attachment.mask_a);
        if (!attachment.enable) continue;
        if (attachment.equation_rgb > 4 || attachment.equation_a > 4 || attachment.factor_source_rgb > 18 ||
            attachment.factor_dest_rgb > 18 || attachment.factor_source_a > 18 || attachment.factor_dest_a > 18)
            return false;
        regs.blend.enable[i] = 1;
        auto & blend = regs.blend_per_target[i];
        blend.color_op = Fixed::UnpackBlendEquation(attachment.equation_rgb);
        blend.alpha_op = Fixed::UnpackBlendEquation(attachment.equation_a);
        blend.color_source = Fixed::UnpackBlendFactor(attachment.factor_source_rgb);
        blend.color_dest = Fixed::UnpackBlendFactor(attachment.factor_dest_rgb);
        blend.alpha_source = Fixed::UnpackBlendFactor(attachment.factor_source_a);
        blend.alpha_dest = Fixed::UnpackBlendFactor(attachment.factor_dest_a);
    }
    if (s.xfb_enabled)
    {
        for (size_t i = 0; i < s.xfb_state.layouts.size(); ++i)
        {
            auto & control = regs.transform_feedback.controls[i];
            control.stream = s.xfb_state.layouts[i].stream;
            control.varying_count = s.xfb_state.layouts[i].varying_count;
            control.stride = s.xfb_state.layouts[i].stride;
        }
        regs.stream_out_layout = s.xfb_state.varyings;
    }
    return true;
}
std::vector<u32> Capture(const Maxwell & regs, u32 topology, u32 hint)
{
    return GuestPipelineState::Capture(regs, static_cast<Maxwell::PrimitiveTopology>(topology), static_cast<EngineHint>(hint))
        .Words();
}
} // namespace

std::optional<Registers> FromWords(std::span<const u32> words)
{
    const auto state = GuestPipelineState::FromWords(words);
    if (!state || state->complete.topology.Value() > Maxwell::PrimitiveTopology::Patches) return std::nullopt;
    const auto canonical = state->Words();
    ZeroedRegisters zeroed;
    u32 topology{}, hint{};
    if (!Write(*state, *zeroed.regs, topology, hint) || Capture(*zeroed.regs, topology, hint) != canonical) return std::nullopt;
    Registers result;
    for (u32 i = 0; i < RegisterCount; ++i)
        if (const auto value = Word(*zeroed.regs, i)) result.emplace_back(i, value);
    if (topology) result.emplace_back(TopologyKey, topology);
    if (hint) result.emplace_back(EngineHintKey, hint);
    return result;
}

std::optional<std::vector<u32>> ToWords(const Registers & registers)
{
    ZeroedRegisters zeroed;
    u32 topology{}, hint{};
    u32 previous{};
    for (size_t i = 0; i < registers.size(); ++i)
    {
        const auto [key, value] = registers[i];
        if ((i && key <= previous) || value == 0) return std::nullopt;
        previous = key;
        if (key < RegisterCount)
            Word(*zeroed.regs, key) = value;
        else if (key == TopologyKey && value <= static_cast<u32>(Maxwell::PrimitiveTopology::Patches))
            topology = value;
        else if (key == EngineHintKey && value <= static_cast<u32>(EngineHint::OnHLEMacro))
            hint = value;
        else
            return std::nullopt;
    }
    // Registers outside the capture are ignored. Field values the capture
    // cannot represent leave words that FromWords refuses.
    auto words = Capture(*zeroed.regs, topology, hint);
    const auto state = GuestPipelineState::FromWords(words);
    if (!state) return std::nullopt;
    return state->Words();
}

u32 TextureTypeCode(u32 type)
{
    using T = Shader::TextureType;
    using Tic = Tegra::Texture::TextureType;
    const auto code = [](Tic tic, bool normalized = true) { return static_cast<u32>(tic) | (normalized ? 16U : 0U); };
    switch (static_cast<T>(type))
    {
    case T::Color1D: return code(Tic::Texture1D);
    case T::ColorArray1D: return code(Tic::Texture1DArray);
    case T::Color2D: return code(Tic::Texture2D);
    case T::ColorArray2D: return code(Tic::Texture2DArray);
    case T::Color3D: return code(Tic::Texture3D);
    case T::ColorCube: return code(Tic::TextureCubemap);
    case T::ColorArrayCube: return code(Tic::TextureCubeArray);
    case T::Buffer: return code(Tic::Texture1DBuffer);
    case T::Color2DRect: return code(Tic::Texture2D, false);
    }
    return code(Tic::Texture2D);
}

std::optional<u32> ShaderTextureType(u32 code)
{
    using T = Shader::TextureType;
    using Tic = Tegra::Texture::TextureType;
    if (code >= 32) return std::nullopt;
    const bool normalized = (code & 16) != 0;
    switch (static_cast<Tic>(code & 15))
    {
    case Tic::Texture1D: return static_cast<u32>(T::Color1D);
    case Tic::Texture2D:
    case Tic::Texture2DNoMipmap: return static_cast<u32>(normalized ? T::Color2D : T::Color2DRect);
    case Tic::Texture3D: return static_cast<u32>(T::Color3D);
    case Tic::TextureCubemap: return static_cast<u32>(T::ColorCube);
    case Tic::Texture1DArray: return static_cast<u32>(T::ColorArray1D);
    case Tic::Texture2DArray: return static_cast<u32>(T::ColorArray2D);
    case Tic::Texture1DBuffer: return static_cast<u32>(T::Buffer);
    case Tic::TextureCubeArray: return static_cast<u32>(T::ColorArrayCube);
    }
    return std::nullopt;
}

std::optional<u32> TextureFormatCode(u32 pixel_format)
{
    const auto entry = TextureDescriptorFromPixelFormat(static_cast<VideoCore::Surface::PixelFormat>(pixel_format));
    if (!entry) return std::nullopt;
    return static_cast<u32>(entry->format) | static_cast<u32>(entry->red) << 7 | static_cast<u32>(entry->green) << 10 |
           static_cast<u32>(entry->blue) << 13 | static_cast<u32>(entry->alpha) << 16 | (entry->srgb ? 1U << 19 : 0U);
}

std::optional<u32> PixelFormat(u32 code)
{
    if (code >> 20) return std::nullopt;
    using Tegra::Texture::ComponentType;
    const auto component = [&](unsigned shift) { return static_cast<ComponentType>((code >> shift) & 7); };
    const auto format = FindPixelFormat(static_cast<Tegra::Texture::TextureFormat>(code & 127), component(7), component(10),
                                        component(13), component(16), (code >> 19) & 1);
    if (!format) return std::nullopt;
    return static_cast<u32>(*format);
}
} // namespace VideoCommon::ShaderProfile::GuestState
