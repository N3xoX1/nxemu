// Frozen pre-refactor renderer Refresh body, GPL-2.0-or-later; independent test oracle.
struct LegacyState : Vulkan::FixedPipelineState {
void RefreshReference(const Maxwell& regs, Maxwell::PrimitiveTopology topology_, Tegra::Engines::Maxwell3D::EngineHint hint, const DynamicFeatures& features, FixedPipelineState::RefreshFlags& dirty) {

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
                attribute.enabled.Assign(input.constant ? 0 : 1);
                attribute.buffer.Assign(input.buffer);
                attribute.offset.Assign(input.offset);
                attribute.type.Assign(static_cast<u32>(input.type.Value()));
                attribute.size.Assign(static_cast<u32>(input.size.Value()));
                if (!input.constant) {
                    read_streams |= 1U << input.buffer;
                }
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
        for (size_t i=0;i<xfb_state.layouts.size();++i) { const auto& c=regs.transform_feedback.controls[i]; xfb_state.layouts[i]={c.stream,c.varying_count,c.stride}; } xfb_state.varyings=regs.stream_out_layout;
    }
}

};
