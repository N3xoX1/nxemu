// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "yuzu_common/bit_cast.h"
#include "yuzu_common/cityhash.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/polyfill_ranges.h"
#include "yuzu_video_core/engines/draw_manager.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"

namespace Vulkan {
namespace {
constexpr size_t POINT = 0;
constexpr size_t LINE = 1;
constexpr size_t POLYGON = 2;
constexpr std::array POLYGON_OFFSET_ENABLE_LUT = {
    POINT,   // Points
    LINE,    // Lines
    LINE,    // LineLoop
    LINE,    // LineStrip
    POLYGON, // Triangles
    POLYGON, // TriangleStrip
    POLYGON, // TriangleFan
    POLYGON, // Quads
    POLYGON, // QuadStrip
    POLYGON, // Polygon
    LINE,    // LinesAdjacency
    LINE,    // LineStripAdjacency
    POLYGON, // TrianglesAdjacency
    POLYGON, // TriangleStripAdjacency
    POLYGON, // Patches
};

void RefreshXfbState(VideoCommon::TransformFeedbackState& state, const Maxwell& regs) {
    std::ranges::transform(regs.transform_feedback.controls, state.layouts.begin(),
                           [](const auto& layout) {
                               return VideoCommon::TransformFeedbackState::Layout{
                                   .stream = layout.stream,
                                   .varying_count = layout.varying_count,
                                   .stride = layout.stride,
                               };
                           });
    state.varyings = regs.stream_out_layout;
}
} // Anonymous namespace

bool FixedPipelineState::UsesDualSourceBlend(const Maxwell& regs) noexcept {
    const auto format = regs.rt[0].format;
    if (!regs.blend.enable[0] || format == Tegra::RenderTargetFormat::NONE ||
        VideoCore::Surface::IsPixelFormatInteger(
            VideoCore::Surface::PixelFormatFromRenderTargetFormat(format))) {
        return false;
    }
    const auto is_second_source = [](Maxwell::Blend::Factor factor) {
        using Factor = Maxwell::Blend::Factor;
        switch (factor) {
        case Factor::Source1Color_D3D:
        case Factor::OneMinusSource1Color_D3D:
        case Factor::Source1Alpha_D3D:
        case Factor::OneMinusSource1Alpha_D3D:
        case Factor::Source1Color_GL:
        case Factor::OneMinusSource1Color_GL:
        case Factor::Source1Alpha_GL:
        case Factor::OneMinusSource1Alpha_GL:
            return true;
        default:
            return false;
        }
    };
    const auto uses_second_source = [&](const auto& blend) {
        return is_second_source(blend.color_source) || is_second_source(blend.color_dest) ||
               is_second_source(blend.alpha_source) || is_second_source(blend.alpha_dest);
    };
    return regs.blend_per_target_enabled ? uses_second_source(regs.blend_per_target[0])
                                         : uses_second_source(regs.blend);
}

void FixedPipelineState::Refresh(Tegra::Engines::Maxwell3D& maxwell3d, DynamicFeatures& features) {
    RefreshFlags dirty{
        maxwell3d.dirty.flags[Dirty::VertexInput],
        maxwell3d.dirty.flags[Dirty::ViewportSwizzles],
        maxwell3d.dirty.flags[Dirty::Blending],
        maxwell3d.dirty.flags[Dirty::BlendEnable],
    };
    RefreshFromRegisters(maxwell3d.regs, maxwell3d.draw_manager->GetDrawState().topology,
                         maxwell3d.engine_state, features, dirty);
    maxwell3d.dirty.flags[Dirty::VertexInput] = dirty.vertex_input;
    maxwell3d.dirty.flags[Dirty::ViewportSwizzles] = dirty.viewport_swizzles;
    maxwell3d.dirty.flags[Dirty::Blending] = dirty.blending;
    maxwell3d.dirty.flags[Dirty::BlendEnable] = dirty.blend_enable;
}

void FixedPipelineState::RefreshFromRegisters(const Maxwell& regs,
    Maxwell::PrimitiveTopology topology_, Tegra::Engines::Maxwell3D::EngineHint hint,
    const DynamicFeatures& features, RefreshFlags& dirty) {

    raw1 = 0;
    extended_dynamic_state.Assign(features.has_extended_dynamic_state ? 1 : 0);
    extended_dynamic_state_2.Assign(features.has_extended_dynamic_state_2 ? 1 : 0);
    extended_dynamic_state_2_extra.Assign(features.has_extended_dynamic_state_2_extra ? 1 : 0);
    extended_dynamic_state_3_blend.Assign(features.has_extended_dynamic_state_3_blend ? 1 : 0);
    extended_dynamic_state_3_enables.Assign(features.has_extended_dynamic_state_3_enables ? 1 : 0);
    dynamic_vertex_input.Assign(features.has_dynamic_vertex_input ? 1 : 0);
    xfb_enabled.Assign(regs.transform_feedback_enabled != 0);
    ndc_minus_one_to_one.Assign(regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1 : 0);
    polygon_mode.Assign(PackPolygonMode(regs.polygon_mode_front));
    tessellation_primitive.Assign(static_cast<u32>(regs.tessellation.params.domain_type.Value()));
    tessellation_spacing.Assign(static_cast<u32>(regs.tessellation.params.spacing.Value()));
    tessellation_clockwise.Assign(regs.tessellation.params.output_primitives.Value() ==
                                  Maxwell::Tessellation::OutputPrimitives::Triangles_CW);
    patch_control_points_minus_one.Assign(regs.patch_vertices - 1);
    topology.Assign(topology_);
    msaa_mode.Assign(regs.anti_alias_samples_mode);

    const bool cached_dual_source_blend = dual_source_blend != 0;
    raw2 = 0;
    dual_source_blend.Assign(cached_dual_source_blend);
    vertex_stride_workaround.Assign(features.has_vertex_stride_workaround &&
                                    !features.has_dynamic_vertex_input ? 1 : 0);

    const auto test_func =
        regs.alpha_test_enabled != 0 ? regs.alpha_test_func : Maxwell::ComparisonOp::Always_GL;
    alpha_test_func.Assign(PackComparisonOp(test_func));
    early_z.Assign(regs.mandated_early_z != 0 ? 1 : 0);
    depth_enabled.Assign(regs.zeta_enable != 0 ? 1 : 0);
    depth_format.Assign(static_cast<u32>(regs.zeta.format));
    NormalizeInactiveFields();
    y_negate.Assign(regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft ? 1 : 0);
    provoking_vertex_last.Assign(regs.provoking_vertex == Maxwell::ProvokingVertex::Last ? 1 : 0);
    conservative_raster_enable.Assign(regs.conservative_raster_enable != 0 ? 1 : 0);
    smooth_lines.Assign(regs.line_anti_alias_enable != 0 ? 1 : 0);
    alpha_to_coverage_enabled.Assign(regs.anti_alias_alpha_control.alpha_to_coverage != 0 ? 1 : 0);
    alpha_to_one_enabled.Assign(regs.anti_alias_alpha_control.alpha_to_one != 0 ? 1 : 0);
    app_stage.Assign(hint);

    bool color_formats_changed = false;
    for (size_t i = 0; i < regs.rt.size(); ++i) {
        const u8 format = static_cast<u8>(regs.rt[i].format);
        color_formats_changed |= color_formats[i] != format;
        color_formats[i] = format;
    }
    if (extended_dynamic_state_3_blend && color_formats_changed) {
        // The dynamic blend enable depends on the render target formats.
        dirty.blending = true;
        dirty.blend_enable = true;
    }
    alpha_test_ref = Common::BitCast<u32>(regs.alpha_test_ref);
    point_size = Common::BitCast<u32>(regs.point_size);

    if (dirty.vertex_input) {
        if (features.has_dynamic_vertex_input) {
            // Dirty flag will be reset by the command buffer update
            static constexpr std::array LUT{
                0u, // Invalid
                1u, // SignedNorm
                1u, // UnsignedNorm
                2u, // SignedInt
                3u, // UnsignedInt
                1u, // UnsignedScaled
                1u, // SignedScaled
                1u, // Float
            };
            const auto& attrs = regs.vertex_attrib_format;
            attribute_types = 0;
            for (size_t i = 0; i < Maxwell::NumVertexAttributes; ++i) {
                const u32 mask = attrs[i].constant != 0 ? 0 : 3;
                const u32 type = LUT[static_cast<size_t>(attrs[i].type.Value())];
                attribute_types |= static_cast<u64>(type & mask) << (i * 2);
            }
        } else {
            dirty.vertex_input = false;
            u32 read_streams = 0;
            for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
                const auto& input = regs.vertex_attrib_format[index];
                auto& attribute = attributes[index];
                attribute.raw = 0;
                // Constant attributes are delivered through current attribute values,
                // not through a VkVertexInputAttributeDescription. Their stale format,
                // stream and offset must not distinguish otherwise identical PSOs.
                if (input.constant) {
                    continue;
                }
                attribute.enabled.Assign(1);
                attribute.buffer.Assign(input.buffer);
                attribute.offset.Assign(input.offset);
                attribute.type.Assign(static_cast<u32>(input.type.Value()));
                attribute.size.Assign(static_cast<u32>(input.size.Value()));
                read_streams |= 1U << input.buffer;
            }
            // The step rate of a stream no attribute reads does not change the draw, so it is
            // left out of the key instead of building another pipeline for it.
            enabled_divisors = 0;
            for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
                const bool is_enabled = ((read_streams >> index) & 1) != 0 &&
                                        regs.vertex_stream_instances.IsInstancingEnabled(index);
                binding_divisors[index] = is_enabled ? regs.vertex_streams[index].frequency : 0;
                enabled_divisors |= (is_enabled ? u64{1} : 0) << index;
            }
        }
    }
    if (dirty.viewport_swizzles) {
        dirty.viewport_swizzles = false;
        const auto& transform = regs.viewport_transform;
        std::ranges::transform(transform, viewport_swizzles.begin(), [](const auto& viewport) {
            return static_cast<u16>(viewport.swizzle.raw);
        });
    }
    dynamic_state.raw1 = 0;
    dynamic_state.raw2 = 0;
    if (!extended_dynamic_state) {
        dynamic_state.Refresh(regs);
        std::ranges::transform(regs.vertex_streams, vertex_strides.begin(), [](const auto& array) {
            return static_cast<u16>(array.stride.Value());
        });
    } else if (vertex_stride_workaround) {
        // Strides stay dynamic, but they select the vertex attribute formats. Disabled streams
        // are zeroed so their stale strides do not create redundant pipelines.
        std::ranges::transform(regs.vertex_streams, vertex_strides.begin(), [](const auto& array) {
            return static_cast<u16>(array.enable != 0 ? array.stride.Value() : 0);
        });
    }
    if (!extended_dynamic_state_2_extra) {
        dynamic_state.Refresh2(regs, topology_, extended_dynamic_state_2);
    }
    if (!extended_dynamic_state_3_blend) {
        if (dirty.blending) {
            dirty.blending = false;
            for (size_t index = 0; index < attachments.size(); ++index) {
                attachments[index].Refresh(regs, index);
            }
        }
    }
    if (!extended_dynamic_state_3_enables) {
        dynamic_state.Refresh3(regs);
    }
    // The shader output interface is static even when the blend equation is dynamic.
    // Only this mode belongs in the key; ordinary blend factors remain dynamic.
    if (!extended_dynamic_state_3_blend || color_formats_changed ||
        dirty.blending || dirty.blend_enable) {
        dual_source_blend.Assign(UsesDualSourceBlend(regs));
    }
    if (xfb_enabled) {
        RefreshXfbState(xfb_state, regs);
    }
}

void GuestPipelineState::Track(Tegra::Engines::Maxwell3D::DirtyState & dirty)
{
    using Regs = Tegra::Engines::Maxwell3D::Regs;
    auto & mask = dirty.shader_profile_registers;
#define TRACK(field) std::fill_n(mask.begin() + offsetof(Regs, field) / 4, sizeof(Regs{}.field) / 4, u8{1})
#define TRACK_GROUP(field, group) std::fill_n(mask.begin() + offsetof(Regs, field) / 4, sizeof(Regs{}.field) / 4, u8{group})
    TRACK_GROUP(transform_feedback.controls, Xfb);
    TRACK_GROUP(stream_out_layout, Xfb);
    TRACK_GROUP(transform_feedback_enabled, Xfb);
    TRACK(depth_mode);
    TRACK(polygon_mode_front);
    TRACK(tessellation.params);
    TRACK(patch_vertices);
    TRACK(anti_alias_samples_mode);
    TRACK(alpha_test_enabled);
    TRACK(alpha_test_func);
    TRACK(alpha_test_ref);
    TRACK(mandated_early_z);
    TRACK(zeta_enable);
    TRACK(zeta.format);
    TRACK(window_origin);
    TRACK(provoking_vertex);
    TRACK(conservative_raster_enable);
    TRACK(line_anti_alias_enable);
    TRACK(anti_alias_alpha_control);
    TRACK(point_size);
    TRACK_GROUP(vertex_attrib_format, VertexInput);
    TRACK_GROUP(vertex_stream_instances, VertexInput);
    TRACK_GROUP(blend.color_op, Blend);
    TRACK_GROUP(blend.color_source, Blend);
    TRACK_GROUP(blend.color_dest, Blend);
    TRACK_GROUP(blend.alpha_op, Blend);
    TRACK_GROUP(blend.alpha_source, Blend);
    TRACK_GROUP(blend.alpha_dest, Blend);
    TRACK_GROUP(blend.enable, Blend);
    // separate_alpha, color-key/ROP controls and padding are not read by the
    // canonical pipeline projection; changes to them must not trigger capture.
    for(size_t i=0;i<Regs::NumRenderTargets;++i)
        for(size_t field:{offsetof(Regs::BlendPerTarget,color_op),offsetof(Regs::BlendPerTarget,color_source),
            offsetof(Regs::BlendPerTarget,color_dest),offsetof(Regs::BlendPerTarget,alpha_op),
            offsetof(Regs::BlendPerTarget,alpha_source),offsetof(Regs::BlendPerTarget,alpha_dest)})
            mask[(offsetof(Regs,blend_per_target)+i*sizeof(Regs::BlendPerTarget)+field)/4]=Blend;
    TRACK_GROUP(blend_per_target_enabled, Blend);
    TRACK_GROUP(color_mask, Blend);
    TRACK_GROUP(color_mask_common, Blend);
    TRACK_GROUP(stencil_front_op, Dynamic);
    TRACK_GROUP(stencil_back_op, Dynamic);
    TRACK_GROUP(stencil_two_side_enable, Dynamic);
    TRACK_GROUP(stencil_enable, Dynamic);
    TRACK_GROUP(depth_write_enabled, Dynamic);
    TRACK_GROUP(depth_bounds_enable, Dynamic);
    TRACK_GROUP(depth_test_enable, Dynamic);
    TRACK_GROUP(depth_test_func, Dynamic);
    TRACK_GROUP(gl_front_face, Dynamic);
    TRACK_GROUP(gl_cull_face, Dynamic);
    TRACK_GROUP(gl_cull_test_enabled, Dynamic);
    TRACK_GROUP(logic_op, Dynamic);
    TRACK_GROUP(rasterize_enable, Dynamic);
    TRACK_GROUP(primitive_restart.enabled, Dynamic);
    TRACK_GROUP(polygon_offset_point_enable, Dynamic);
    TRACK_GROUP(polygon_offset_line_enable, Dynamic);
    TRACK_GROUP(polygon_offset_fill_enable, Dynamic);
    TRACK_GROUP(viewport_clip_control, Dynamic);
    for (size_t i = 0; i < Regs::NumVertexArrays; ++i)
    {
        mask[(offsetof(Regs, vertex_streams) + i * sizeof(Regs::VertexStream)) / 4] = Strides;
        mask[(offsetof(Regs, vertex_streams) + i * sizeof(Regs::VertexStream) + offsetof(Regs::VertexStream, frequency)) / 4] = VertexInput;
    }
    for (size_t i = 0; i < 8; ++i)
        mask[(offsetof(Regs, rt) + i * sizeof(Regs{}.rt[0]) + offsetof(std::remove_reference_t<decltype(Regs{}.rt[0])>, format)) / 4] = 1;
    for (size_t i = 0; i < 16; ++i)
        mask[(offsetof(Regs, viewport_transform) + i * sizeof(Regs{}.viewport_transform[0]) + offsetof(std::remove_reference_t<decltype(Regs{}.viewport_transform[0])>, swizzle)) / 4] = Viewport;
#undef TRACK
#undef TRACK_GROUP
}

GuestPipelineState GuestPipelineState::Capture(const Maxwell & regs,
                                               Maxwell::PrimitiveTopology topology, Tegra::Engines::Maxwell3D::EngineHint hint)
{
    GuestPipelineState result;
    FixedPipelineState::RefreshFlags dirty{true, true, true, true};
    result.complete.RefreshFromRegisters(regs, topology, hint, DynamicFeatures{}, dirty);
    for (size_t index = 0; index < regs.vertex_streams.size(); ++index) {
        if (regs.vertex_streams[index].enable != 0) {
            result.enabled_vertex_streams |= 1U << index;
        }
    }
    return result;
}

FixedPipelineState GuestPipelineState::Project(const DynamicFeatures& features) const {
    FixedPipelineState result = complete;
    result.NormalizeInactiveFields();
    result.extended_dynamic_state.Assign(features.has_extended_dynamic_state);
    result.extended_dynamic_state_2.Assign(features.has_extended_dynamic_state_2);
    result.extended_dynamic_state_2_extra.Assign(features.has_extended_dynamic_state_2_extra);
    result.extended_dynamic_state_3_blend.Assign(features.has_extended_dynamic_state_3_blend);
    result.extended_dynamic_state_3_enables.Assign(features.has_extended_dynamic_state_3_enables);
    result.dynamic_vertex_input.Assign(features.has_dynamic_vertex_input);
    const bool stride_workaround = features.has_vertex_stride_workaround &&
                                   !features.has_dynamic_vertex_input;
    result.vertex_stride_workaround.Assign(stride_workaround);
    // These masks select the fields written by the existing DynamicState Refresh methods.
    u32 mask1 = features.has_extended_dynamic_state ? 0U : 0x7U;
    u32 mask2 = features.has_extended_dynamic_state ? 0U : UINT32_MAX;
    if (!features.has_extended_dynamic_state_2_extra) {
        mask1 |= features.has_extended_dynamic_state_2 ? 0x3c0U : 0x3f8U;
    }
    if (!features.has_extended_dynamic_state_3_enables) {
        mask1 |= 0xc00U;
    }
    result.dynamic_state.raw1 &= mask1;
    result.dynamic_state.raw2 &= mask2;
    if (features.has_extended_dynamic_state_3_blend) {
        result.attachments = {};
    }
    if (features.has_dynamic_vertex_input) {
        static constexpr std::array types{0U, 1U, 1U, 2U, 3U, 1U, 1U, 1U};
        result.attribute_types = 0;
        for (size_t i = 0; i < complete.attributes.size(); ++i) {
            const auto& attr = complete.attributes[i];
            if (attr.enabled) {
                result.attribute_types |= u64{types[static_cast<size_t>(attr.Type())]} << (i * 2);
            }
        }
        result.attributes = {};
        result.binding_divisors = {};
    }
    if (features.has_extended_dynamic_state) {
        for (size_t i = 0; i < result.vertex_strides.size(); ++i) {
            if (!stride_workaround || (enabled_vertex_streams & (1U << i)) == 0) {
                result.vertex_strides[i] = 0;
            }
        }
    }
    return result;
}

std::vector<u32> GuestPipelineState::Words() const {
    std::vector<u32> words(WordCount);
    WriteWords(std::span<u32, WordCount>(words.data(), words.size()));
    return words;
}

void GuestPipelineState::WriteWords(std::span<u32, WordCount> words) const {
    size_t cursor{};
    const auto push = [&](u32 value) { words[cursor++] = value; };
    const auto& s = complete;
    push(1U); push(s.raw1); push(s.raw2); push(enabled_vertex_streams);
    for (auto x : s.color_formats) push(x);
    push(s.alpha_test_ref);
    push(s.point_size);
    for (auto x : s.viewport_swizzles) push(x);
    push(static_cast<u32>(s.enabled_divisors));
    push(static_cast<u32>(s.enabled_divisors >> 32));
    push(s.dynamic_state.raw1);
    push(s.dynamic_state.raw2);
    for (auto x : s.attachments) push(x.raw);
    for (auto x : s.attributes) push(x.raw);
    for (auto x : s.binding_divisors) push(x);
    for (auto x : s.vertex_strides) push(x);
    for (auto x : s.xfb_state.layouts) {
        push(x.stream); push(x.varying_count); push(x.stride);
    }
    for (const auto& stream : s.xfb_state.varyings) {
        for (const auto& x : stream) {
            push(x.attribute0.Value() | (x.attribute1.Value() << 8) |
                            (x.attribute2.Value() << 16) | (x.attribute3.Value() << 24));
        }
    }
    ASSERT(cursor == WordCount);
}

std::optional<GuestPipelineState> GuestPipelineState::FromWords(std::span<const u32> words) {
    GuestPipelineState result;
    if (words.size() != WordCount || words[0] != 1) return std::nullopt;
    size_t cursor = 1;
    const auto next = [&]() { return words[cursor++]; };
    auto& s = result.complete;
    s.raw1 = next();
    s.raw2 = next();
    result.enabled_vertex_streams = next();
    if ((s.raw1 & 63U) != 0 || s.vertex_stride_workaround != 0) return std::nullopt;
    for (auto& x : s.color_formats) x = static_cast<u8>(next());
    s.alpha_test_ref = next();
    s.point_size = next();
    for (auto& x : s.viewport_swizzles) x = static_cast<u16>(next());
    const u64 low = next();
    s.enabled_divisors = low | (u64{next()} << 32);
    s.dynamic_state.raw1 = next();
    s.dynamic_state.raw2 = next();
    for (auto& x : s.attachments) x.raw = next();
    for (auto& x : s.attributes) x.raw = next();
    for (auto& x : s.binding_divisors) x = next();
    for (auto& x : s.vertex_strides) x = static_cast<u16>(next());
    for (auto& x : s.xfb_state.layouts) {
        x.stream = next(); x.varying_count = next(); x.stride = next();
    }
    for (auto& stream : s.xfb_state.varyings) {
        for (auto& x : stream) {
            const u32 packed = next();
            x.attribute0.Assign(packed & 255U);
            x.attribute1.Assign((packed >> 8) & 255U);
            x.attribute2.Assign((packed >> 16) & 255U);
            x.attribute3.Assign(packed >> 24);
        }
    }
    // Reject values truncated by narrower fields rather than silently changing a recipe.
    std::array<u32, WordCount> canonical;
    result.WriteWords(canonical);
    if (!std::equal(words.begin(), words.end(), canonical.begin())) return std::nullopt;
    return result;
}

void FixedPipelineState::BlendingAttachment::Refresh(const Maxwell& regs, size_t index) {
    const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];

    raw = 0;
    mask_r.Assign(mask.R);
    mask_g.Assign(mask.G);
    mask_b.Assign(mask.B);
    mask_a.Assign(mask.A);

    // TODO: C++20 Use templated lambda to deduplicate code
    if (!regs.blend.enable[index]) {
        return;
    }

    const auto setup_blend = [&]<typename T>(const T& src) {
        equation_rgb.Assign(PackBlendEquation(src.color_op));
        equation_a.Assign(PackBlendEquation(src.alpha_op));
        factor_source_rgb.Assign(PackBlendFactor(src.color_source));
        factor_dest_rgb.Assign(PackBlendFactor(src.color_dest));
        factor_source_a.Assign(PackBlendFactor(src.alpha_source));
        factor_dest_a.Assign(PackBlendFactor(src.alpha_dest));
        enable.Assign(1);
    };

    if (!regs.blend_per_target_enabled) {
        setup_blend(regs.blend);
        return;
    }
    setup_blend(regs.blend_per_target[index]);
}

void FixedPipelineState::DynamicState::Refresh(const Maxwell& regs) {
    u32 packed_front_face = PackFrontFace(regs.gl_front_face);
    if (regs.window_origin.flip_y != 0) {
        // Flip front face
        packed_front_face = 1 - packed_front_face;
    }

    front.action_stencil_fail.Assign(PackStencilOp(regs.stencil_front_op.fail));
    front.action_depth_fail.Assign(PackStencilOp(regs.stencil_front_op.zfail));
    front.action_depth_pass.Assign(PackStencilOp(regs.stencil_front_op.zpass));
    front.test_func.Assign(PackComparisonOp(regs.stencil_front_op.func));
    if (regs.stencil_two_side_enable) {
        back.action_stencil_fail.Assign(PackStencilOp(regs.stencil_back_op.fail));
        back.action_depth_fail.Assign(PackStencilOp(regs.stencil_back_op.zfail));
        back.action_depth_pass.Assign(PackStencilOp(regs.stencil_back_op.zpass));
        back.test_func.Assign(PackComparisonOp(regs.stencil_back_op.func));
    } else {
        back.action_stencil_fail.Assign(front.action_stencil_fail);
        back.action_depth_fail.Assign(front.action_depth_fail);
        back.action_depth_pass.Assign(front.action_depth_pass);
        back.test_func.Assign(front.test_func);
    }
    stencil_enable.Assign(regs.stencil_enable);
    depth_write_enable.Assign(regs.depth_write_enabled);
    depth_bounds_enable.Assign(regs.depth_bounds_enable);
    depth_test_enable.Assign(regs.depth_test_enable);
    front_face.Assign(packed_front_face);
    depth_test_func.Assign(PackComparisonOp(regs.depth_test_func));
    cull_face.Assign(PackCullFace(regs.gl_cull_face));
    cull_enable.Assign(regs.gl_cull_test_enabled != 0 ? 1 : 0);
}

void FixedPipelineState::DynamicState::Refresh2(const Maxwell& regs,
                                                Maxwell::PrimitiveTopology topology_,
                                                bool base_features_supported) {
    logic_op.Assign(PackLogicOp(regs.logic_op.op));

    if (base_features_supported) {
        return;
    }

    const std::array enabled_lut{
        regs.polygon_offset_point_enable,
        regs.polygon_offset_line_enable,
        regs.polygon_offset_fill_enable,
    };
    const u32 topology_index = static_cast<u32>(topology_);

    rasterize_enable.Assign(regs.rasterize_enable != 0 ? 1 : 0);
    primitive_restart_enable.Assign(regs.primitive_restart.enabled != 0 ? 1 : 0);
    depth_bias_enable.Assign(enabled_lut[POLYGON_OFFSET_ENABLE_LUT[topology_index]] != 0 ? 1 : 0);
}

void FixedPipelineState::DynamicState::Refresh3(const Maxwell& regs) {
    logic_op_enable.Assign(regs.logic_op.enable != 0 ? 1 : 0);
    depth_clamp_disabled.Assign(regs.viewport_clip_control.geometry_clip ==
                                    Maxwell::ViewportClipControl::GeometryClip::Passthrough ||
                                regs.viewport_clip_control.geometry_clip ==
                                    Maxwell::ViewportClipControl::GeometryClip::FrustumXYZ ||
                                regs.viewport_clip_control.geometry_clip ==
                                    Maxwell::ViewportClipControl::GeometryClip::FrustumZ);
}

size_t FixedPipelineState::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), Size());
    return static_cast<size_t>(hash);
}

bool FixedPipelineState::operator==(const FixedPipelineState& rhs) const noexcept {
    return std::memcmp(this, &rhs, Size()) == 0;
}

u32 FixedPipelineState::PackComparisonOp(Maxwell::ComparisonOp op) noexcept {
    // OpenGL enums go from 0x200 to 0x207 and the others from 1 to 8
    // If we subtract 0x200 to OpenGL enums and 1 to the others we get a 0-7 range.
    // Perfect for a hash.
    const u32 value = static_cast<u32>(op);
    return value - (value >= 0x200 ? 0x200 : 1);
}

Maxwell::ComparisonOp FixedPipelineState::UnpackComparisonOp(u32 packed) noexcept {
    // Read PackComparisonOp for the logic behind this.
    return static_cast<Maxwell::ComparisonOp>(packed + 1);
}

u32 FixedPipelineState::PackStencilOp(Maxwell::StencilOp::Op op) noexcept {
    switch (op) {
    case Maxwell::StencilOp::Op::Keep_D3D:
    case Maxwell::StencilOp::Op::Keep_GL:
        return 0;
    case Maxwell::StencilOp::Op::Zero_D3D:
    case Maxwell::StencilOp::Op::Zero_GL:
        return 1;
    case Maxwell::StencilOp::Op::Replace_D3D:
    case Maxwell::StencilOp::Op::Replace_GL:
        return 2;
    case Maxwell::StencilOp::Op::IncrSaturate_D3D:
    case Maxwell::StencilOp::Op::IncrSaturate_GL:
        return 3;
    case Maxwell::StencilOp::Op::DecrSaturate_D3D:
    case Maxwell::StencilOp::Op::DecrSaturate_GL:
        return 4;
    case Maxwell::StencilOp::Op::Invert_D3D:
    case Maxwell::StencilOp::Op::Invert_GL:
        return 5;
    case Maxwell::StencilOp::Op::Incr_D3D:
    case Maxwell::StencilOp::Op::Incr_GL:
        return 6;
    case Maxwell::StencilOp::Op::Decr_D3D:
    case Maxwell::StencilOp::Op::Decr_GL:
        return 7;
    }
    return 0;
}

Maxwell::StencilOp::Op FixedPipelineState::UnpackStencilOp(u32 packed) noexcept {
    static constexpr std::array LUT = {
        Maxwell::StencilOp::Op::Keep_D3D,         Maxwell::StencilOp::Op::Zero_D3D,
        Maxwell::StencilOp::Op::Replace_D3D,      Maxwell::StencilOp::Op::IncrSaturate_D3D,
        Maxwell::StencilOp::Op::DecrSaturate_D3D, Maxwell::StencilOp::Op::Invert_D3D,
        Maxwell::StencilOp::Op::Incr_D3D,         Maxwell::StencilOp::Op::Decr_D3D};
    return LUT[packed];
}

u32 FixedPipelineState::PackCullFace(Maxwell::CullFace cull) noexcept {
    // FrontAndBack is 0x408, by subtracting 0x406 in it we get 2.
    // Individual cull faces are in 0x404 and 0x405, subtracting 0x404 we get 0 and 1.
    const u32 value = static_cast<u32>(cull);
    return value - (value == 0x408 ? 0x406 : 0x404);
}

Maxwell::CullFace FixedPipelineState::UnpackCullFace(u32 packed) noexcept {
    static constexpr std::array LUT = {Maxwell::CullFace::Front, Maxwell::CullFace::Back,
                                       Maxwell::CullFace::FrontAndBack};
    return LUT[packed];
}

u32 FixedPipelineState::PackFrontFace(Maxwell::FrontFace face) noexcept {
    return static_cast<u32>(face) - 0x900;
}

Maxwell::FrontFace FixedPipelineState::UnpackFrontFace(u32 packed) noexcept {
    return static_cast<Maxwell::FrontFace>(packed + 0x900);
}

u32 FixedPipelineState::PackPolygonMode(Maxwell::PolygonMode mode) noexcept {
    return static_cast<u32>(mode) - 0x1B00;
}

Maxwell::PolygonMode FixedPipelineState::UnpackPolygonMode(u32 packed) noexcept {
    return static_cast<Maxwell::PolygonMode>(packed + 0x1B00);
}

u32 FixedPipelineState::PackLogicOp(Maxwell::LogicOp::Op op) noexcept {
    return static_cast<u32>(op) - 0x1500;
}

Maxwell::LogicOp::Op FixedPipelineState::UnpackLogicOp(u32 packed) noexcept {
    return static_cast<Maxwell::LogicOp::Op>(packed + 0x1500);
}

u32 FixedPipelineState::PackBlendEquation(Maxwell::Blend::Equation equation) noexcept {
    switch (equation) {
    case Maxwell::Blend::Equation::Add_D3D:
    case Maxwell::Blend::Equation::Add_GL:
        return 0;
    case Maxwell::Blend::Equation::Subtract_D3D:
    case Maxwell::Blend::Equation::Subtract_GL:
        return 1;
    case Maxwell::Blend::Equation::ReverseSubtract_D3D:
    case Maxwell::Blend::Equation::ReverseSubtract_GL:
        return 2;
    case Maxwell::Blend::Equation::Min_D3D:
    case Maxwell::Blend::Equation::Min_GL:
        return 3;
    case Maxwell::Blend::Equation::Max_D3D:
    case Maxwell::Blend::Equation::Max_GL:
        return 4;
    }
    return 0;
}

Maxwell::Blend::Equation FixedPipelineState::UnpackBlendEquation(u32 packed) noexcept {
    static constexpr std::array LUT = {
        Maxwell::Blend::Equation::Add_D3D, Maxwell::Blend::Equation::Subtract_D3D,
        Maxwell::Blend::Equation::ReverseSubtract_D3D, Maxwell::Blend::Equation::Min_D3D,
        Maxwell::Blend::Equation::Max_D3D};
    return LUT[packed];
}

u32 FixedPipelineState::PackBlendFactor(Maxwell::Blend::Factor factor) noexcept {
    switch (factor) {
    case Maxwell::Blend::Factor::Zero_D3D:
    case Maxwell::Blend::Factor::Zero_GL:
        return 0;
    case Maxwell::Blend::Factor::One_D3D:
    case Maxwell::Blend::Factor::One_GL:
        return 1;
    case Maxwell::Blend::Factor::SourceColor_D3D:
    case Maxwell::Blend::Factor::SourceColor_GL:
        return 2;
    case Maxwell::Blend::Factor::OneMinusSourceColor_D3D:
    case Maxwell::Blend::Factor::OneMinusSourceColor_GL:
        return 3;
    case Maxwell::Blend::Factor::SourceAlpha_D3D:
    case Maxwell::Blend::Factor::SourceAlpha_GL:
        return 4;
    case Maxwell::Blend::Factor::OneMinusSourceAlpha_D3D:
    case Maxwell::Blend::Factor::OneMinusSourceAlpha_GL:
        return 5;
    case Maxwell::Blend::Factor::DestAlpha_D3D:
    case Maxwell::Blend::Factor::DestAlpha_GL:
        return 6;
    case Maxwell::Blend::Factor::OneMinusDestAlpha_D3D:
    case Maxwell::Blend::Factor::OneMinusDestAlpha_GL:
        return 7;
    case Maxwell::Blend::Factor::DestColor_D3D:
    case Maxwell::Blend::Factor::DestColor_GL:
        return 8;
    case Maxwell::Blend::Factor::OneMinusDestColor_D3D:
    case Maxwell::Blend::Factor::OneMinusDestColor_GL:
        return 9;
    case Maxwell::Blend::Factor::SourceAlphaSaturate_D3D:
    case Maxwell::Blend::Factor::SourceAlphaSaturate_GL:
        return 10;
    case Maxwell::Blend::Factor::Source1Color_D3D:
    case Maxwell::Blend::Factor::Source1Color_GL:
        return 11;
    case Maxwell::Blend::Factor::OneMinusSource1Color_D3D:
    case Maxwell::Blend::Factor::OneMinusSource1Color_GL:
        return 12;
    case Maxwell::Blend::Factor::Source1Alpha_D3D:
    case Maxwell::Blend::Factor::Source1Alpha_GL:
        return 13;
    case Maxwell::Blend::Factor::OneMinusSource1Alpha_D3D:
    case Maxwell::Blend::Factor::OneMinusSource1Alpha_GL:
        return 14;
    case Maxwell::Blend::Factor::BlendFactor_D3D:
    case Maxwell::Blend::Factor::ConstantColor_GL:
        return 15;
    case Maxwell::Blend::Factor::OneMinusBlendFactor_D3D:
    case Maxwell::Blend::Factor::OneMinusConstantColor_GL:
        return 16;
    case Maxwell::Blend::Factor::BothSourceAlpha_D3D:
    case Maxwell::Blend::Factor::ConstantAlpha_GL:
        return 17;
    case Maxwell::Blend::Factor::OneMinusBothSourceAlpha_D3D:
    case Maxwell::Blend::Factor::OneMinusConstantAlpha_GL:
        return 18;
    }
    UNIMPLEMENTED_MSG("Unknown blend factor {}", static_cast<u32>(factor));
    return 0;
}

Maxwell::Blend::Factor FixedPipelineState::UnpackBlendFactor(u32 packed) noexcept {
    static constexpr std::array LUT = {
        Maxwell::Blend::Factor::Zero_D3D,
        Maxwell::Blend::Factor::One_D3D,
        Maxwell::Blend::Factor::SourceColor_D3D,
        Maxwell::Blend::Factor::OneMinusSourceColor_D3D,
        Maxwell::Blend::Factor::SourceAlpha_D3D,
        Maxwell::Blend::Factor::OneMinusSourceAlpha_D3D,
        Maxwell::Blend::Factor::DestAlpha_D3D,
        Maxwell::Blend::Factor::OneMinusDestAlpha_D3D,
        Maxwell::Blend::Factor::DestColor_D3D,
        Maxwell::Blend::Factor::OneMinusDestColor_D3D,
        Maxwell::Blend::Factor::SourceAlphaSaturate_D3D,
        Maxwell::Blend::Factor::Source1Color_D3D,
        Maxwell::Blend::Factor::OneMinusSource1Color_D3D,
        Maxwell::Blend::Factor::Source1Alpha_D3D,
        Maxwell::Blend::Factor::OneMinusSource1Alpha_D3D,
        Maxwell::Blend::Factor::BlendFactor_D3D,
        Maxwell::Blend::Factor::OneMinusBlendFactor_D3D,
        Maxwell::Blend::Factor::BothSourceAlpha_D3D,
        Maxwell::Blend::Factor::OneMinusBothSourceAlpha_D3D,
    };
    ASSERT(packed < LUT.size());
    return LUT[packed];
}

} // namespace Vulkan
