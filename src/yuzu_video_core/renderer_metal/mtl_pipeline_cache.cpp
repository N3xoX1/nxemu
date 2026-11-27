// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <bit>
#include <bitset>
#include <cstdlib>
#include <fstream>
#include <memory>

#include <fmt/format.h>
#include <boost/container/static_vector.hpp>

#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/cityhash.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_video_core/dirty_flags.h"
#include "yuzu_video_core/renderer_metal/metal_options.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/transform_feedback.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/virtual_buffer.h"
#include "yuzu_video_core/shader_notify.h"
#include "yuzu_video_core/renderer_metal/mtl_pipeline_cache.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_shader_recompiler/frontend/maxwell/control_flow.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate_program.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_video_core/engines/kepler_compute.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/shader_environment.h"
#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/renderer_metal/mtl_shader_util.h"

namespace Metal {

namespace {

using Shader::Backend::Bindings;

// Describes Metal to the shader recompiler. Flags the Vulkan backend sets true
// are false here on purpose, each one drops the recompiler onto an emulation
// path it already keeps around for OpenGL and older GPUs.
Shader::Profile MakeProfile(const Device& device) {
    Shader::Profile profile{};

    // SPIR-V 1.3 matches what host_shaders is built against, and it's the
    // highest version SPIRV-Cross takes cleanly for what we need.
    profile.supported_spirv = 0x00010300;

    profile.unified_descriptor_binding = true;
    // Argument buffers dislike aliasing distinct descriptors onto one slot.
    profile.support_descriptor_aliasing = false;

    profile.support_int8 = true;
    profile.support_int16 = true;
    profile.support_int64 = device.SupportsMetal3();

    // MSL has no gl_VertexIndex/gl_InstanceIndex distinction, the recompiler
    // emulates it with base vertex and base instance.
    profile.support_vertex_instance_id = false;

    // No SPV_KHR_float_controls analogue, MSL fixes rounding and denormal
    // behaviour instead of letting the shader pick.
    profile.support_float_controls = false;

    profile.support_vote = true;
    // Fragment and compute only, those are the stages where MSL exposes the
    // SIMD-group functions a subgroup vote lowers to. Claim more and the
    // recompiler emits a vote in a vertex shader, which won't compile.
    profile.support_subgroup_in_vertex_stages = false;
    profile.support_subgroup_in_geometry_stage = false;

    // Apple GPUs are 32 wide, matching the guest, but discrete AMD parts are
    // 64. The recompiler masks ballot results down only when told to.
    profile.warp_size_potentially_larger_than_guest = device.GetSubgroupSize() > 32;

    // [[render_target_array_index]] is writable from a vertex shader on Apple
    // GPUs, so layered rendering doesn't need a geometry stage.
    profile.support_viewport_index_layer_non_geometry = true;
    profile.support_multi_viewport = true;

    // Apple Silicon (Read-Write Texture Tier 2) supports reading storage textures
    // without an explicit format qualifier in MSL (matching Vulkan's
    // shaderStorageImageReadWithoutFormat). Leaving this false replaces every
    // typeless image read (e.g. RE Engine / Monster Hunter Rise) with 0.
    profile.support_typeless_image_loads =
        device.GetDevice()->readWriteTextureSupport() >= MTL::ReadWriteTextureTier2;

    profile.support_demote_to_helper_invocation = true;
    profile.support_int64_atomics = false;
    profile.support_derivative_control = false;

    // Metal has neither geometry shaders nor their passthrough extension.
    // Geometry stages are handled at the IR level via FindForwardedLayerAttribute
    // and VtgVertexToCompute + VtgGeometryToCompute.
    profile.support_geometry_shader_passthrough = false;
    profile.support_geometry_streams = false;

    // Metal clip space is 0..1 only, so we have to emit the depth range fixup.
    profile.support_native_ndc = false;
    profile.lower_left_origin_mode = false;

    // Tier 2 argument buffers let us index a resource array with a value that
    // varies across the quad. All four kinds need it, leave any false and the
    // recompiler emits a scalar fallback loop.
    const bool nonuniform_indexing = device.SupportsArgumentBuffersTier2();
    profile.support_sampled_image_array_nonuniform_indexing = nonuniform_indexing;
    profile.support_storage_image_array_nonuniform_indexing = nonuniform_indexing;
    profile.support_uniform_texel_buffer_array_nonuniform_indexing = nonuniform_indexing;

    // No NV_viewport_array2 equivalent, a vertex shader can write one viewport
    // index and not a mask.
    profile.support_viewport_mask = false;

    // Apple GPUs round the gather coordinate the way Maxwell does, so the
    // half-texel correction some desktop GPUs need would shift every gather.
    profile.need_gather_subpixel_offset = false;

    // MSL wants a complete fragment output struct, undeclared colour outputs
    // read as undefined otherwise.
    profile.need_declared_frag_colors = true;

    // Fast math changes NaN and infinity handling in ways guest shaders notice.
    profile.need_fastmath_off = true;

    // Apple GPU behaviour NxEmu already measured through MoltenVK: unorm8
    // render targets round half up, NoContraction in fragment shaders makes the
    // Metal compiler slow, inlined global memory helpers make compute builds
    // pathological, and Metal refuses kernels over its shared memory limit.
    profile.has_unorm8_round_half_up = true;
    profile.has_slow_fragment_no_contraction = true;
    profile.avoid_global_memory_helper_inlining = true;
    profile.max_compute_shared_memory_size = 32768;

    // Two views per texel buffer, see BufferCacheRuntime::BindTextureBuffer.
    profile.texel_buffer_size_view = true;

    profile.min_ssbo_alignment = 16;
    profile.max_user_clip_distances = 8;
    profile.max_subgroup_size = device.GetSubgroupSize();

    return profile;
}

Shader::HostTranslateInfo MakeHostTranslateInfo(const Device& device) {
    return Shader::HostTranslateInfo{
        .support_float64 = false, // MSL has no double precision at all.
        .support_float16 = true,  // half is native and fast on Apple GPUs.
        .support_int64 = device.SupportsMetal3(),
        .needs_demote_reorder = false,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = true,
        .min_ssbo_alignment = 16,
        .support_geometry_shader_passthrough = false,
        // Apple Silicon / Metal supports threadgroup_barrier inside loops and uniform
        // conditional branches (matching Vulkan/MoltenVK). Leaving this false runs
        // ConditionalBarrierPass, which deletes Barrier instructions inside loops/ifs
        // and causes threadgroup memory races (flickering/corrupted tiles).
        .support_conditional_barrier = true,
        // Tier 2 argument buffers support 500,000+ textures/samplers/buffers in a
        // single argument buffer; capping at 64 rejected RE Engine's bindless
        // texture arrays (e.g. Monster Hunter Rise: 929-2644 descriptors/stage).
        .max_bindless_descriptors_per_stage =
            device.SupportsArgumentBuffersTier2() ? 500000u : 64u,
        .max_storage_images_per_stage =
            device.SupportsArgumentBuffersTier2() ? 500000u : 64u,
        // Metal backs NULL_IMAGE_VIEW_ID with resident 1x1x1 ShaderRead|ShaderWrite dummy
        // textures whose out-of-bounds writes are discarded by hardware, matching Vulkan's
        // nullDescriptor semantics for runtime storage image tables.
        .support_null_descriptor = true,
        .support_separate_sampler = true,
        .support_compute_direct_textures = true,
    };
}

// The recompiler's spelling of a Maxwell comparison, for the alpha test it bakes
// into the fragment shader.
Shader::CompareFunction MaxwellToCompareFunction(
    Tegra::Engines::Maxwell3D::Regs::ComparisonOp comparison) {
    using Op = Tegra::Engines::Maxwell3D::Regs::ComparisonOp;
    switch (comparison) {
    case Op::Never_D3D:
    case Op::Never_GL:
        return Shader::CompareFunction::Never;
    case Op::Less_D3D:
    case Op::Less_GL:
        return Shader::CompareFunction::Less;
    case Op::Equal_D3D:
    case Op::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Op::LessEqual_D3D:
    case Op::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Op::Greater_D3D:
    case Op::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Op::NotEqual_D3D:
    case Op::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Op::GreaterEqual_D3D:
    case Op::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Op::Always_D3D:
    case Op::Always_GL:
        return Shader::CompareFunction::Always;
    default:
        return Shader::CompareFunction::Always;
    }
}

// Everything the recompiler needs that isn't in the guest bytecode. Taken from
// the pipeline key and never from live Maxwell registers, so a pipeline can be
// rebuilt outside the draw that first wanted it, which is what the disk shader
// cache is for. Every value here has to be in the key too, they all change the
// compiled shader.
/// Declares the float color attachments a fragment shader reads at its own pixel as
/// input attachments, which SPIRV-Cross turns into framebuffer fetch.
void FindColorAttachmentReads(Shader::IR::Program& program, const FixedPipelineState& state,
                              u32 screen_position_generics) {
    if (!Shader::Optimization::FindPixelLocalTextureReads(program, screen_position_generics)) {
        return;
    }
    u32 mask{};
    for (size_t index = 0; index < state.color_attachments.size(); ++index) {
        if (state.color_attachments[index].format != MTL::PixelFormatInvalid &&
            state.color_output_types[index] == 0) {
            mask |= 1U << index;
        }
    }
    if (mask == 0) {
        for (auto& desc : program.info.texture_descriptors) {
            desc.pixel_local_read = Shader::PixelLocalRead::None;
        }
        return;
    }
    program.info.input_attachment_mask = mask;
}

Shader::InputTopology MaxwellToInputTopology(
    Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology) {
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    switch (topology) {
    case Topology::Points:
        return Shader::InputTopology::Points;
    case Topology::Lines:
    case Topology::LineLoop:
    case Topology::LineStrip:
        return Shader::InputTopology::Lines;
    case Topology::Triangles:
    case Topology::TriangleStrip:
    case Topology::TriangleFan:
    case Topology::Quads:
    case Topology::QuadStrip:
    case Topology::Polygon:
    case Topology::Patches:
        return Shader::InputTopology::Triangles;
    case Topology::LinesAdjacency:
    case Topology::LineStripAdjacency:
        return Shader::InputTopology::LinesAdjacency;
    case Topology::TrianglesAdjacency:
    case Topology::TriangleStripAdjacency:
        return Shader::InputTopology::TrianglesAdjacency;
    }
    return Shader::InputTopology::Triangles;
}

std::optional<Shader::VtgTopology> MaxwellToVtgTopology(
    Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology) {
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    switch (topology) {
    case Topology::Points:
        return Shader::VtgTopology::Points;
    case Topology::Lines:
        return Shader::VtgTopology::Lines;
    case Topology::LineLoop:
        return Shader::VtgTopology::LineLoop;
    case Topology::LineStrip:
        return Shader::VtgTopology::LineStrip;
    case Topology::Triangles:
        return Shader::VtgTopology::Triangles;
    case Topology::TriangleStrip:
        return Shader::VtgTopology::TriangleStrip;
    case Topology::TriangleFan:
    case Topology::Polygon:
        return Shader::VtgTopology::TriangleFan;
    case Topology::Quads:
        return Shader::VtgTopology::Quads;
    case Topology::QuadStrip:
        return Shader::VtgTopology::QuadStrip;
    case Topology::LinesAdjacency:
        return Shader::VtgTopology::LinesAdjacency;
    case Topology::LineStripAdjacency:
        return Shader::VtgTopology::LineStripAdjacency;
    case Topology::TrianglesAdjacency:
        return Shader::VtgTopology::TrianglesAdjacency;
    case Topology::TriangleStripAdjacency:
        return Shader::VtgTopology::TriangleStripAdjacency;
    case Topology::Patches:
        break;
    }
    return std::nullopt;
}

Shader::VtgVertexInputs MakeVtgVertexInputs(const FixedPipelineState& state) {
    Shader::VtgVertexInputs inputs{};
    for (size_t index = 0; index < inputs.size(); ++index) {
        const FixedPipelineState::VertexAttribute& attribute = state.attributes[index];
        const MTL::VertexFormat format = attribute.format;
        const auto attr_type = static_cast<Shader::AttributeType>(state.attribute_types[index]);
        if (format == MTL::VertexFormatInvalid || attr_type == Shader::AttributeType::Disabled) {
            continue;
        }
        Shader::VtgVertexAttribute& input = inputs[index];
        input.buffer = static_cast<u8>(attribute.buffer_index);
        const bool is_float = attr_type == Shader::AttributeType::Float ||
                              attr_type == Shader::AttributeType::UnsignedScaled ||
                              attr_type == Shader::AttributeType::SignedScaled;
        const auto set = [&](Shader::VtgComponentType type, u8 components, u8 bits) {
            input.type = type;
            input.components = components;
            input.bits = bits;
        };
        const auto uint_type =
            is_float ? Shader::VtgComponentType::UScaled : Shader::VtgComponentType::UInt;
        const auto sint_type =
            is_float ? Shader::VtgComponentType::SScaled : Shader::VtgComponentType::SInt;

        switch (format) {
        case MTL::VertexFormatUCharNormalized: set(Shader::VtgComponentType::UNorm, 1, 8); break;
        case MTL::VertexFormatUChar2Normalized: set(Shader::VtgComponentType::UNorm, 2, 8); break;
        case MTL::VertexFormatUChar3Normalized: set(Shader::VtgComponentType::UNorm, 3, 8); break;
        case MTL::VertexFormatUChar4Normalized: set(Shader::VtgComponentType::UNorm, 4, 8); break;
        case MTL::VertexFormatCharNormalized: set(Shader::VtgComponentType::SNorm, 1, 8); break;
        case MTL::VertexFormatChar2Normalized: set(Shader::VtgComponentType::SNorm, 2, 8); break;
        case MTL::VertexFormatChar3Normalized: set(Shader::VtgComponentType::SNorm, 3, 8); break;
        case MTL::VertexFormatChar4Normalized: set(Shader::VtgComponentType::SNorm, 4, 8); break;
        case MTL::VertexFormatUChar: set(uint_type, 1, 8); break;
        case MTL::VertexFormatUChar2: set(uint_type, 2, 8); break;
        case MTL::VertexFormatUChar3: set(uint_type, 3, 8); break;
        case MTL::VertexFormatUChar4: set(uint_type, 4, 8); break;
        case MTL::VertexFormatChar: set(sint_type, 1, 8); break;
        case MTL::VertexFormatChar2: set(sint_type, 2, 8); break;
        case MTL::VertexFormatChar3: set(sint_type, 3, 8); break;
        case MTL::VertexFormatChar4: set(sint_type, 4, 8); break;

        case MTL::VertexFormatUShortNormalized: set(Shader::VtgComponentType::UNorm, 1, 16); break;
        case MTL::VertexFormatUShort2Normalized: set(Shader::VtgComponentType::UNorm, 2, 16); break;
        case MTL::VertexFormatUShort3Normalized: set(Shader::VtgComponentType::UNorm, 3, 16); break;
        case MTL::VertexFormatUShort4Normalized: set(Shader::VtgComponentType::UNorm, 4, 16); break;
        case MTL::VertexFormatShortNormalized: set(Shader::VtgComponentType::SNorm, 1, 16); break;
        case MTL::VertexFormatShort2Normalized: set(Shader::VtgComponentType::SNorm, 2, 16); break;
        case MTL::VertexFormatShort3Normalized: set(Shader::VtgComponentType::SNorm, 3, 16); break;
        case MTL::VertexFormatShort4Normalized: set(Shader::VtgComponentType::SNorm, 4, 16); break;
        case MTL::VertexFormatUShort: set(uint_type, 1, 16); break;
        case MTL::VertexFormatUShort2: set(uint_type, 2, 16); break;
        case MTL::VertexFormatUShort3: set(uint_type, 3, 16); break;
        case MTL::VertexFormatUShort4: set(uint_type, 4, 16); break;
        case MTL::VertexFormatShort: set(sint_type, 1, 16); break;
        case MTL::VertexFormatShort2: set(sint_type, 2, 16); break;
        case MTL::VertexFormatShort3: set(sint_type, 3, 16); break;
        case MTL::VertexFormatShort4: set(sint_type, 4, 16); break;
        case MTL::VertexFormatHalf: set(Shader::VtgComponentType::Float, 1, 16); break;
        case MTL::VertexFormatHalf2: set(Shader::VtgComponentType::Float, 2, 16); break;
        case MTL::VertexFormatHalf3: set(Shader::VtgComponentType::Float, 3, 16); break;
        case MTL::VertexFormatHalf4: set(Shader::VtgComponentType::Float, 4, 16); break;

        case MTL::VertexFormatUInt: set(uint_type, 1, 32); break;
        case MTL::VertexFormatUInt2: set(uint_type, 2, 32); break;
        case MTL::VertexFormatUInt3: set(uint_type, 3, 32); break;
        case MTL::VertexFormatUInt4: set(uint_type, 4, 32); break;
        case MTL::VertexFormatInt: set(sint_type, 1, 32); break;
        case MTL::VertexFormatInt2: set(sint_type, 2, 32); break;
        case MTL::VertexFormatInt3: set(sint_type, 3, 32); break;
        case MTL::VertexFormatInt4: set(sint_type, 4, 32); break;
        case MTL::VertexFormatFloat: set(Shader::VtgComponentType::Float, 1, 32); break;
        case MTL::VertexFormatFloat2: set(Shader::VtgComponentType::Float, 2, 32); break;
        case MTL::VertexFormatFloat3: set(Shader::VtgComponentType::Float, 3, 32); break;
        case MTL::VertexFormatFloat4: set(Shader::VtgComponentType::Float, 4, 32); break;

        case MTL::VertexFormatUInt1010102Normalized:
            set(Shader::VtgComponentType::UNorm, 4, 0);
            input.packed_offsets = {0, 10, 20, 30};
            input.packed_bits = {10, 10, 10, 2};
            break;
        case MTL::VertexFormatInt1010102Normalized:
            set(Shader::VtgComponentType::SNorm, 4, 0);
            input.packed_offsets = {0, 10, 20, 30};
            input.packed_bits = {10, 10, 10, 2};
            break;
        case MTL::VertexFormatFloatRG11B10:
            set(Shader::VtgComponentType::Float, 3, 0);
            input.packed_offsets = {0, 11, 22, 0};
            input.packed_bits = {11, 11, 10, 0};
            break;
        default:
            break;
        }
    }
    return inputs;
}

Shader::RuntimeInfo MakeRuntimeInfo(
    const FixedPipelineState& state, const Shader::IR::Program& program,
    const Shader::IR::Program* previous_program, bool has_geometry,
    std::optional<Shader::IR::Attribute> forwarded_layer = std::nullopt,
    const std::array<Shader::TransformFeedbackVarying, 256>* xfb_varyings = nullptr,
    u32 xfb_count = 0) {
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;
    Shader::RuntimeInfo info;
    if (previous_program != nullptr) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }

    const float point_size = std::bit_cast<float>(state.point_size);
    const bool draws_points = state.topology_class == MTL::PrimitiveTopologyClassPoint;

    switch (program.stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (draws_points) {
                info.fixed_state_point_size = point_size;
            }
            // Metal clip space is 0..1 only, so a guest using OpenGL-style -1..1
            // NDC needs the depth conversion emitted into the shader.
            info.convert_depth_mode = state.ndc_minus_one_to_one;
            // The constant the guest's shaders read as YDirection.
            info.y_negate = state.y_direction_negate;
            info.forwarded_layer_attribute = forwarded_layer;
        }
        // Every vertex input has to be declared with the type the vertex
        // descriptor actually delivers. A mismatch reads an integer attribute as
        // a float, or the other way round, and says nothing.
        for (size_t index = 0; index < state.attribute_types.size(); ++index) {
            info.generic_input_types[index] =
                static_cast<Shader::AttributeType>(state.attribute_types[index]);
        }
        break;
    case Shader::Stage::TessellationEval:
        // Domain, spacing and winding are execution modes on the evaluation
        // program so the recompiler needs them up front. This doesn't make
        // tessellation work, both stages are still dropped below, it only makes
        // the SPIR-V describe the guest's actual domain.
        info.tess_clockwise = state.tessellation_clockwise != 0;
        info.tess_primitive = [&state] {
            switch (static_cast<Maxwell::Tessellation::DomainType>(state.tessellation_domain)) {
            case Maxwell::Tessellation::DomainType::Isolines:
                return Shader::TessPrimitive::Isolines;
            case Maxwell::Tessellation::DomainType::Triangles:
                return Shader::TessPrimitive::Triangles;
            case Maxwell::Tessellation::DomainType::Quads:
                return Shader::TessPrimitive::Quads;
            }
            return Shader::TessPrimitive::Triangles;
        }();
        info.tess_spacing = [&state] {
            switch (static_cast<Maxwell::Tessellation::Spacing>(state.tessellation_spacing)) {
            case Maxwell::Tessellation::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case Maxwell::Tessellation::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
            case Maxwell::Tessellation::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            }
            return Shader::TessSpacing::Equal;
        }();
        break;
    case Shader::Stage::Geometry:
        info.input_topology =
            MaxwellToInputTopology(static_cast<Maxwell::PrimitiveTopology>(state.topology));
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        // Geometry is the last stage before rasterisation, so the -1..1 -> 0..1
        // clip space conversion belongs here and not in the vertex shader.
        // EmitEmitVertex applies it per emitted vertex.
        info.convert_depth_mode = state.ndc_minus_one_to_one;
        // The constant the guest's shaders read as YDirection.
        info.y_negate = state.y_direction_negate;
        break;
    case Shader::Stage::Fragment: {
        // Early-Z is an execution mode baked into the shader so it's part of the
        // key. Force it ONLY where it can't change results. Under
        // EarlyFragmentTests a discarding shader still writes depth for the
        // texels it threw away, and that geometry then occludes everything
        // behind it.
        //
        // The discard test has to include the alpha test. The SPIR-V backend
        // emits that as an OpKill after collect_shader_info_pass has run, so
        // uses_demote_to_helper_invocation is false for exactly the alpha-tested
        // surfaces that make this dangerous. Saying no costs nothing, the
        // hardware may apply early-Z anyway.
        const auto alpha_op =
            static_cast<Tegra::Engines::Maxwell3D::Regs::ComparisonOp>(state.alpha_test_func);
        const bool alpha_test_discards =
            alpha_op != Tegra::Engines::Maxwell3D::Regs::ComparisonOp::Always_GL &&
            alpha_op != Tegra::Engines::Maxwell3D::Regs::ComparisonOp::Always_D3D;
        info.alpha_to_coverage_dither = state.EmulatesAlphaToCoverageDither();
        const bool dither_discards =
            info.alpha_to_coverage_dither && program.info.stores_frag_alpha[0];
        const bool alpha_to_coverage_discards =
            state.alpha_to_coverage != 0 && program.info.stores_frag_alpha[0];
        const bool may_discard = program.info.uses_demote_to_helper_invocation ||
                                 alpha_test_discards || dither_discards ||
                                 alpha_to_coverage_discards || program.info.stores_sample_mask;
        // Apple Silicon TBDR enables Early-Z automatically for side-effect-free
        // fragment shaders, but forces Late-Z when a shader writes to a storage
        // image without [[early_fragment_tests]]. On M-series GPUs, Late-Z depth
        // interpolation can differ by ~0.5 ULP from the Z-prepass's Early-Z/HSR
        // depth, causing read-only Equal depth passes to reject pixels in a moire
        // pattern (e.g. Immortals Fenyx Rising virtual-texturing rocks).
        // Scope this strictly to storage-image writes on read-only depth-Equal
        // passes: forcing [[early_fragment_tests]] on SSBO-writing G-buffer and
        // deferred passes (such as TOTK's tile-classification / title mural
        // shaders) suppresses their SSBO writes and breaks tiled shading.
        const bool has_storage_image_writes =
            program.info.runtime_image_write_mask_words != 0 ||
            std::ranges::any_of(program.info.image_descriptors,
                                [](const auto& desc) { return desc.is_written; }) ||
            std::ranges::any_of(program.info.image_buffer_descriptors,
                                [](const auto& desc) { return desc.is_written; });
        info.force_early_z =
            (state.force_early_z || (state.depth_equal_readonly && has_storage_image_writes)) &&
            !may_discard && !program.info.stores_frag_depth;
        // Alpha test is fixed function on Maxwell and Metal has no equivalent,
        // so the recompiler emits it. Without this every alpha-tested surface
        // draws as a solid quad.
        info.alpha_test_func = MaxwellToCompareFunction(
            static_cast<Tegra::Engines::Maxwell3D::Regs::ComparisonOp>(state.alpha_test_func));
        info.alpha_test_reference = std::bit_cast<float>(state.alpha_test_ref);

        info.dual_source_blend = state.dual_source_blend != 0;
        info.quantize_d24_frag_depth = state.guest_depth_d24 && program.info.stores_frag_depth;

        // MSL wants a fragment output's type to match its attachment's format.
        // Writing float4 into an integer attachment is a hard compilation error,
        // not a conversion.
        for (size_t index = 0; index < info.color_output_types.size(); ++index) {
            info.color_output_types[index] =
                static_cast<Shader::AttributeType>(state.color_output_types[index]);
            const MTL::PixelFormat fmt = state.color_attachments[index].format;
            info.color_output_unorm8[index] =
                fmt == MTL::PixelFormatRGBA8Unorm || fmt == MTL::PixelFormatRGBA8Unorm_sRGB ||
                fmt == MTL::PixelFormatBGRA8Unorm || fmt == MTL::PixelFormatBGRA8Unorm_sRGB ||
                fmt == MTL::PixelFormatR8Unorm || fmt == MTL::PixelFormatRG8Unorm;
        }
        break;
    }
    default:
        break;
    }

    // How many vertices one geometry shader invocation reads.
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    switch (static_cast<Topology>(state.topology)) {
    case Topology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Topology::Lines:
    case Topology::LineLoop:
    case Topology::LineStrip:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Topology::LinesAdjacency:
    case Topology::LineStripAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Topology::TrianglesAdjacency:
    case Topology::TriangleStripAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    default:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    }

    // No caller passes these right now, see the note in the stage loop on why the
    // capture path translates without xfb decorations on purpose.
    if (xfb_varyings != nullptr && program.stage == Shader::Stage::VertexB) {
        info.xfb_varyings = *xfb_varyings;
        info.xfb_count = xfb_count;
    }
    return info;
}

} // Anonymous namespace

// How the shader has to declare vertex input @p index, given what the vertex
// descriptor will hand it. MUST agree with VertexFormat() in maxwell_to_mtl, a
// disagreement isn't a validation error, the attribute just reads as the wrong
// thing. Scaled types are approximated, Metal has no *_USCALED equivalent so we
// deliver and declare the integer, and a shader wanting the widened float value
// gets an unscaled integer.
Shader::AttributeType AttributeTypeOf(const Tegra::Engines::Maxwell3D::Regs& regs, size_t index) {
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;
    const auto& attribute = regs.vertex_attrib_format[index];
    if (attribute.constant != 0) {
        // Constant attributes are fed from the zero-filled null buffer, but the
        // shader has to declare them Disabled the same way Vulkan does
        // (vk_pipeline_cache.cpp CastAttributeType, off the enabled bit that
        // fixed_pipeline_state.cpp assigns as `input.constant ? 0 : 1`).
        //
        // Declare Float instead and it reads the null buffer and gives
        // (0,0,0,0). Disabled makes the recompiler substitute the architectural
        // default (0,0,0,1) at EmitGetAttribute. A mesh whose blend-weight
        // stream is constant collapses toward the origin on the w=0 form.
        return Shader::AttributeType::Disabled;
    }
    switch (attribute.type) {
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        // Scaled isn't integer. The data is integer in memory but the shader
        // reads a float carrying its value, and the recompiler only does that
        // conversion if we tell it the attribute is scaled.
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    default:
        break;
    }
    return Shader::AttributeType::Float;
}

// Whether @p factor references the shader's second colour output.
bool IsDualSourceBlendFactor(Tegra::Engines::Maxwell3D::Regs::Blend::Factor factor) {
    using F = Tegra::Engines::Maxwell3D::Regs::Blend::Factor;
    switch (factor) {
    case F::Source1Color_D3D:
    case F::OneMinusSource1Color_D3D:
    case F::Source1Alpha_D3D:
    case F::OneMinusSource1Alpha_D3D:
    case F::Source1Color_GL:
    case F::OneMinusSource1Color_GL:
    case F::Source1Alpha_GL:
    case F::OneMinusSource1Alpha_GL:
        return true;
    default:
        return false;
    }
}

// Dual-source blending makes the fragment shader declare a second output for
// index 1 instead of a second attachment. That changes the shader's signature so
// it belongs in the pipeline key.
bool ComputeAttachment0DualSourceBlend(const Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!regs.blend.enable[0] || regs.rt[0].format == Tegra::RenderTargetFormat::NONE) {
        return false;
    }
    const auto pixel_format =
        VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[0].format);
    if (VideoCore::Surface::IsPixelFormatInteger(pixel_format)) {
        return false;
    }
    const auto uses_dual_source = [](const auto& blend) {
        return IsDualSourceBlendFactor(blend.color_source) ||
               IsDualSourceBlendFactor(blend.color_dest) ||
               IsDualSourceBlendFactor(blend.alpha_source) ||
               IsDualSourceBlendFactor(blend.alpha_dest);
    };
    return regs.blend_per_target_enabled != 0 ? uses_dual_source(regs.blend_per_target[0])
                                              : uses_dual_source(regs.blend);
}


size_t GraphicsPipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), sizeof(*this));
    return static_cast<size_t>(hash);
}

size_t ComputePipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), sizeof(*this));
    return static_cast<size_t>(hash);
}

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             const Device& device_, Scheduler& scheduler_,
                             VideoCore::ShaderNotify& shader_notify_)
    : VideoCommon::ShaderCache{device_memory_}, device{device_}, scheduler{scheduler_},
      shader_notify{shader_notify_},
      use_asynchronous_shaders{videoSettings.use_asynchronous_shaders},
      profile{MakeProfile(device_)}, host_info{MakeHostTranslateInfo(device_)},
      depth_stencil_cache{device_}, serialization_thread{1, "MtlPipelineSerialization"} {
    graphics_profile = profile;
    graphics_profile.avoid_global_memory_helper_inlining = false;
    const size_t num_workers = std::max<size_t>(
        1, static_cast<size_t>(std::thread::hardware_concurrency()) - 1);
    workers = std::make_unique<ShaderWorkerPool>(num_workers, "MetalShaderBuilder");
    last_archive_flush = std::chrono::steady_clock::now();
}

void PipelineCache::MaybeFlushMetal4Archive(bool graphics_added, bool compute_added) {
    // ~PipelineCache used to be the only writer, so any hard kill, crash or
    // force-quit left the archive at zero bytes and the next session rebuilt
    // every pipeline from MSL.
    //
    // serializeAsArchiveAndFlushToURL rewrites the archive under an exclusive
    // per-stage compiler lock, so keep mid-session flushes infrequent AND only run
    // them when the worker pool is idle (zero stalls during shader bursts).
    constexpr u32 PIPELINES_PER_FLUSH = 128;
    constexpr auto MIN_FLUSH_INTERVAL = std::chrono::seconds{120};

    if (graphics_added) {
        ++graphics_pipelines_since_archive_flush;
    }
    if (compute_added) {
        ++compute_pipelines_since_archive_flush;
    }
    const u32 total_pending =
        graphics_pipelines_since_archive_flush + compute_pipelines_since_archive_flush;
    if (total_pending < PIPELINES_PER_FLUSH) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_archive_flush != std::chrono::steady_clock::time_point{} &&
        now - last_archive_flush < MIN_FLUSH_INTERVAL) {
        return;
    }
    // Defer while shader workers are actively compiling a burst; keep the counters
    // armed so the flush runs as soon as the pool goes idle (or at shutdown).
    if (workers && !workers->IsIdle()) {
        return;
    }
    const bool save_graphics = graphics_pipelines_since_archive_flush > 0;
    const bool save_compute = compute_pipelines_since_archive_flush > 0;
    graphics_pipelines_since_archive_flush = 0;
    compute_pipelines_since_archive_flush = 0;
    last_archive_flush = now;
    device.SaveMetal4Archive(save_graphics, save_compute);
}

PipelineCache::~PipelineCache() {
    // Drain the pool before any pipeline is destroyed. The workers abandon queued
    // work on stop_requested(), so an unbuilt pipeline never signals completion
    // and ~GraphicsPipeline waits for exactly that signal.
    if (workers) {
        workers->WaitForRequests();
    }
    serialization_thread.WaitForRequests();
    // After the drain, so everything this session compiled is in the serializer
    // before it gets written out.
    device.SaveMetal4Archive();
    ClearLibraryCache(device.GetDevice() ? device.GetDevice()->registryID() : 0);
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0) {
        return;
    }
    // Return temporary warmup heap pages (SPIR-V/IR/MSL) to the OS once loading finishes,
    // matching Vulkan's PipelineCache::LoadDiskResources without running on live gameplay frames.
    SCOPE_EXIT {
        Common::PurgeHostHeap();
    };
    // Two caches doing different jobs. The Metal 4 archive makes *recompiling* a
    // pipeline cheap (handed back through the compiler task options), the file
    // below remembers *which* pipelines the title needs, key plus guest
    // bytecode, so we can rebuild them before the game asks.
    device.LoadMetal4Archive(title_id);

    const auto shader_dir = Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir);
    const auto base_dir = shader_dir / fmt::format("{:016x}", title_id);
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create the pipeline cache directories");
        return;
    }
    pipeline_cache_filename = base_dir / "metal.bin";

    struct {
        std::mutex cache_mutex;
        std::mutex callback_mutex;
        std::chrono::steady_clock::time_point last_callback{};
        size_t total{};
        std::atomic<size_t> built{0};
        std::atomic<bool> has_loaded{false};
    } state;

    const auto report_progress = [&state, &callback]() {
        const size_t built = state.built.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (!state.has_loaded.load(std::memory_order_acquire)) {
            return;
        }
        // Non-blocking try_lock + 60 Hz throttle so UI progress notifications never
        // serialize shader builder workers or hold cache_mutex.
        if (std::unique_lock cb_lock{state.callback_mutex, std::try_to_lock}; cb_lock.owns_lock()) {
            const auto now = std::chrono::steady_clock::now();
            if (built >= state.total ||
                now - state.last_callback >= std::chrono::milliseconds{16}) {
                state.last_callback = now;
                callback(VideoCore::LoadCallbackStage::Build, built, state.total);
            }
        }
    };

    const auto load_compute = [&](std::istream& file, VideoCommon::FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        workers->QueueWork(
            [this, key, env_ = std::move(env), &state, &report_progress]() mutable {
                // Reused per worker thread to avoid 8192-inst ObjectPool churn on every pipeline.
                thread_local ShaderPools pools;
                auto pipeline = CreateComputePipeline(pools, key, env_, false);

                if (pipeline) {
                    std::scoped_lock lock{state.cache_mutex};
                    cbuf_size_dependencies.Observe(key.unique_hash, env_.CbufSizeMask());
                    Vulkan::CbufSizeDependencies::Normalize(
                        key.cbuf_sizes, cbuf_size_dependencies.Mask(key.unique_hash));
                    compute_cache.emplace(key, std::move(pipeline));
                }
                report_progress();
            });
        ++state.total;
        return true;
    };

    const auto load_graphics = [&](std::istream& file,
                                   std::vector<VideoCommon::FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));

        workers->QueueWork(
            [this, key, envs_ = std::move(envs), &state, &report_progress]() mutable {
                // Reused per worker thread to avoid 8192-inst ObjectPool churn on every pipeline.
                thread_local ShaderPools pools;

                // Densely packed and not indexed by stage. GraphicsEnvironments::Span
                // stops at the first null entry so a gap would truncate it. The
                // consumer re-pairs entries with stages by walking unique_hashes in
                // serialization order.
                GraphicsEnvironments environments;
                const size_t num_envs = std::min(envs_.size(), environments.env_ptrs.size());
                for (size_t index = 0; index < num_envs; ++index) {
                    environments.env_ptrs[index] = &envs_[index];
                }
                auto pipeline = CreateGraphicsPipeline(pools, key, environments, false);

                if (pipeline) {
                    std::scoped_lock lock{state.cache_mutex};
                    size_t env_idx = 0;
                    for (size_t stage_idx = 0; stage_idx < key.unique_hashes.size(); ++stage_idx) {
                        if (key.unique_hashes[stage_idx] != 0 && env_idx < envs_.size()) {
                            cbuf_size_dependencies.Observe(key.unique_hashes[stage_idx],
                                                           envs_[env_idx++].CbufSizeMask());
                        }
                    }
                    NormalizeGraphicsCbufSizes(key);
                    graphics_cache.emplace(key, std::move(pipeline));
                }
                report_progress();
            });
        ++state.total;
        return true;
    };

    VideoCommon::LoadPipelines(stop_loading, pipeline_cache_filename, CACHE_VERSION, load_compute,
                               load_graphics);
    LOG_INFO(Render_Metal, "Preloading {} pipelines from the shader cache", state.total);
    const auto preload_started = std::chrono::steady_clock::now();
    {
        std::scoped_lock cb_lock{state.callback_mutex};
        state.last_callback = preload_started;
        callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
        state.has_loaded.store(true, std::memory_order_release);
    }

    workers->WaitForRequests(stop_loading);
    {
        std::scoped_lock cb_lock{state.callback_mutex};
        callback(VideoCore::LoadCallbackStage::Build, state.built.load(std::memory_order_acquire),
                 state.total);
    }

    // The only way to tell a warm boot that reused compiled code from one that
    // quietly recompiled the lot.
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - preload_started)
                             .count();
    LOG_INFO(Render_Metal, "Preloaded {} pipelines in {} ms", state.total, elapsed);
}

bool PipelineCache::CanSkipCurrentDraw() const noexcept {
    if (wait_for_current_draw || !use_asynchronous_shaders || maxwell3d == nullptr) {
        return false;
    }
    // A transform feedback draw fills a buffer the next draws read as vertices. Skipping it
    // leaves whatever an earlier capture put there, so those draws come out as spikes.
    if (maxwell3d->regs.transform_feedback_enabled != 0) {
        return false;
    }
    // A handful of vertices is almost always a full-screen quad building a texture later
    // draws sample, so skipping one corrupts everything that reads the result instead of
    // delaying an object by a frame. Check only the active draw kind (indexed vs non-indexed):
    // the unused count is zero or stale.
    const auto& draw_state = maxwell3d->draw_manager->GetDrawState();
    const u32 count = draw_state.draw_indexed ? draw_state.index_buffer.count
                                              : draw_state.vertex_buffer.count;
    return count > 6;
}

void PipelineCache::SerializeGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                              GraphicsEnvironments& environments) {
    if (pipeline_cache_filename.empty() || key.state.xfb_enabled) {
        return;
    }
    for (auto& env : environments.envs) {
        env.ReleaseSnapshot();
    }
    serialization_thread.QueueWork([this, key, envs = std::move(environments.envs)]() mutable {
        boost::container::static_vector<const VideoCommon::GenericEnvironment*, 5> env_ptrs;
        for (size_t index = 0; index < envs.size(); ++index) {
            if (key.unique_hashes[index] != 0) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        VideoCommon::SerializePipeline(key, env_ptrs, pipeline_cache_filename, CACHE_VERSION);
        MaybeFlushMetal4Archive(/*graphics_added=*/true, /*compute_added=*/false);
    });
}

void PipelineCache::NormalizeGraphicsCbufSizes(GraphicsPipelineCacheKey& key) const {
    for (size_t stage = 0; stage < key.cbuf_sizes.size(); ++stage) {
        const u32 mask = cbuf_size_dependencies.Mask(key.unique_hashes[stage + 1]) |
                         (stage == 0 ? cbuf_size_dependencies.Mask(key.unique_hashes[0]) : 0);
        Vulkan::CbufSizeDependencies::Normalize(key.cbuf_sizes[stage], mask);
    }
    if (std::ranges::none_of(key.unique_hashes, [this](u64 hash) {
            return (cbuf_size_dependencies.Mask(hash) & VideoCommon::TEXTURE_POOL_SIZE_BIT) != 0;
        })) {
        key.texture_pool = {};
    }
}

void PipelineCache::ObserveGraphicsEnvironments(const GraphicsPipelineCacheKey& key,
                                                const GraphicsEnvironments& environments) {
    for (size_t index = 0; index < key.unique_hashes.size(); ++index) {
        if (key.unique_hashes[index] != 0) {
            cbuf_size_dependencies.Observe(key.unique_hashes[index],
                                           environments.envs[index].CbufSizeMask());
        }
    }
}

void PipelineCache::FinishPendingGraphics(const GraphicsPipelineCacheKey& key) {
    const auto it = pending_graphics.find(key);
    if (it == pending_graphics.end()) {
        return;
    }
    auto job = std::move(it->second);
    pending_graphics.erase(it);
    if (job->pipeline) {
        ObserveGraphicsEnvironments(key, job->environments);
    }
    GraphicsPipelineCacheKey normalized = key;
    NormalizeGraphicsCbufSizes(normalized);
    auto& entry = graphics_cache[normalized];
    if (!entry) {
        entry = std::move(job->pipeline);
        if (entry) {
            SerializeGraphicsPipeline(normalized, job->environments);
        }
    }
}

void PipelineCache::DrainCompletedGraphicsPipelines() {
    if (!has_completed_graphics.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<GraphicsPipelineCacheKey> finished;
    {
        std::scoped_lock lock{completed_graphics_mutex};
        finished.swap(completed_graphics);
        has_completed_graphics.store(false, std::memory_order_release);
    }
    for (const GraphicsPipelineCacheKey& key : finished) {
        FinishPendingGraphics(key);
    }
}

bool PipelineCache::QueueGraphicsPipeline(const GraphicsPipelineCacheKey& key) {
    constexpr size_t MAX_PENDING_GRAPHICS = 512;
    if (pending_graphics.size() >= MAX_PENDING_GRAPHICS || maxwell3d == nullptr ||
        gpu_memory == nullptr) {
        return false;
    }
    u32 stage_mask{};
    for (size_t index = 0; index < key.unique_hashes.size(); ++index) {
        if (key.unique_hashes[index] != 0) {
            stage_mask |= 1u << (index == 0 ? 0 : index - 1);
        }
    }
    auto snapshot = VideoCommon::GraphicsEnvironment::CaptureSnapshot(
        *maxwell3d, *gpu_memory, stage_mask, snapshot_cache);
    if (!snapshot) {
        return false;
    }
    auto job = std::make_shared<PendingGraphicsPipeline>();
    job->key = key;
    GetGraphicsEnvironments(job->environments, key.unique_hashes);
    for (size_t index = 0; index < key.unique_hashes.size(); ++index) {
        if (key.unique_hashes[index] != 0) {
            job->environments.envs[index].SetSnapshot(snapshot);
        }
    }
    pending_graphics.emplace(key, job);
    shader_notify.MarkShaderBuilding();
    workers->QueueWork([this, key, job]() mutable {
        thread_local ShaderPools pools;
        job->pipeline = CreateGraphicsPipeline(pools, key, job->environments, false);
        for (auto& env : job->environments.envs) {
            env.ReleaseSnapshot();
        }
        {
            std::scoped_lock lock{job->mutex};
            job->done = true;
        }
        job->cv.notify_all();
        {
            std::scoped_lock lock{completed_graphics_mutex};
            completed_graphics.push_back(key);
            has_completed_graphics.store(true, std::memory_order_release);
        }
        shader_notify.MarkShaderComplete();
    });
    return true;
}

GraphicsPipeline* PipelineCache::WaitForCurrentGraphicsPipeline() {
    if (!skipped_unfinished) {
        return nullptr;
    }
    skipped_unfinished = false;
    wait_for_current_draw = true;
    SCOPE_EXIT {
        wait_for_current_draw = false;
    };
    return CurrentGraphicsPipeline();
}

bool PipelineCache::CanReuseLastLookup(
    Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology,
    u32 hle_replacements) const noexcept {
    const auto& dirty = maxwell3d->dirty;
    return !dirty.pipeline_key_changed && !dirty.flags[VideoCommon::Dirty::Shaders] &&
           !shader_infos_stale.load(std::memory_order_relaxed) &&
           fast_lookup_pipeline != nullptr && fast_lookup_pipeline == current_graphics_pipeline &&
           looked_up_generation == graphics_state_generation && fast_lookup_engine == maxwell3d &&
           fast_lookup_topology == topology &&
           fast_lookup_hle_replacements == hle_replacements &&
           fast_lookup_revision == cbuf_size_dependencies.Revision();
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {
    skipped_unfinished = false;
    DrainCompletedGraphicsPipelines();

    // Without this a pipeline built outside a macro draw is reused inside one, where the base
    // vertex it reads from the constant buffer was never written, and the reverse.
    u32 hle_replacements = 0;
    if (maxwell3d->engine_state == Tegra::Engines::Maxwell3D::EngineHint::OnHLEMacro) {
        for (const auto& [address, type] : maxwell3d->replace_table) {
            hle_replacements |= 1u << static_cast<u32>(type);
        }
    }

    const auto topology = maxwell3d->draw_manager->GetDrawState().topology;
    if (CanReuseLastLookup(topology, hle_replacements)) {
        return BuiltPipeline(current_graphics_pipeline);
    }
    fast_lookup_pipeline = nullptr;
    maxwell3d->dirty.pipeline_key_changed = false;

    const auto finish_lookup = [&](GraphicsPipeline* pipeline) -> GraphicsPipeline* {
        if (pipeline != nullptr && pipeline == current_graphics_pipeline) {
            fast_lookup_pipeline = pipeline;
            fast_lookup_engine = maxwell3d;
            fast_lookup_topology = topology;
            fast_lookup_hle_replacements = hle_replacements;
            fast_lookup_revision = cbuf_size_dependencies.Revision();
        }
        return pipeline;
    };

    const bool shaders_dirty = maxwell3d->dirty.flags[VideoCommon::Dirty::Shaders];
    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_graphics_pipeline = nullptr;
        looked_up_generation = 0;
        return nullptr;
    }
    const auto prev_cbuf_sizes = graphics_key.cbuf_sizes;
    const auto prev_texture_pool = graphics_key.texture_pool;
    graphics_cbuf_size_state.Refresh(
        graphics_key.cbuf_sizes, graphics_key.unique_hashes, cbuf_size_dependencies,
        [&](size_t stage, size_t bank) {
            const auto& buffer = maxwell3d->state.shader_stages[stage].const_buffers[bank];
            return buffer.enabled ? static_cast<u32>(buffer.size) : 0u;
        });
    graphics_key.texture_pool = {};
    if ((graphics_cbuf_size_state.ExtraMask() & VideoCommon::TEXTURE_POOL_SIZE_BIT) != 0) {
        const auto pool = VideoCommon::GraphicsTexturePoolInfo(maxwell3d->regs);
        graphics_key.texture_pool = {pool.num_textures,
                                     pool.linked_samplers ? 0U : pool.num_samplers};
    }
    if (!shaders_dirty && looked_up_generation == graphics_state_generation &&
        hle_replacements == graphics_key.hle_replacements &&
        prev_cbuf_sizes == graphics_key.cbuf_sizes &&
        prev_texture_pool == graphics_key.texture_pool && last_graphics_entry != nullptr) {
        // Same key as the last lookup: same pipeline, which may have finished building since.
        const auto& entry =
            *static_cast<const std::unique_ptr<GraphicsPipeline>*>(last_graphics_entry);
        current_graphics_pipeline = entry && entry->IsValid() ? entry.get() : nullptr;
        return finish_lookup(BuiltPipeline(current_graphics_pipeline));
    }
    looked_up_generation = graphics_state_generation;
    graphics_key.state = graphics_state;
    graphics_key.hle_replacements = hle_replacements;

    std::vector<GraphicsTransition>* transitions = nullptr;
    std::unique_ptr<GraphicsPipeline>* cached = nullptr;
    if (last_graphics_entry != nullptr) {
        transitions = &graphics_transitions[last_graphics_entry];
        for (const GraphicsTransition& transition : *transitions) {
            if (*transition.key == graphics_key) {
                cached = transition.pipeline;
                break;
            }
        }
    }
    if (cached != nullptr) {
        last_graphics_entry = cached;
        current_graphics_pipeline = *cached && (*cached)->IsValid() ? cached->get() : nullptr;
        return finish_lookup(BuiltPipeline(current_graphics_pipeline));
    }

    const auto record_transition = [&](const GraphicsPipelineCacheKey* key_ptr,
                                       std::unique_ptr<GraphicsPipeline>* pipe_ptr) {
        if (transitions != nullptr) {
            constexpr size_t MAX_TRANSITIONS = 8;
            if (transitions->size() >= MAX_TRANSITIONS) {
                transitions->erase(transitions->begin());
            }
            transitions->push_back({key_ptr, pipe_ptr});
        }
        last_graphics_entry = pipe_ptr;
    };

    if (const auto it = graphics_cache.find(graphics_key); it != graphics_cache.end()) {
        record_transition(&it->first, &it->second);
        current_graphics_pipeline =
            it->second && it->second->IsValid() ? it->second.get() : nullptr;
        return finish_lookup(BuiltPipeline(current_graphics_pipeline));
    }

    if (const auto pit = pending_graphics.find(graphics_key); pit != pending_graphics.end()) {
        if (CanSkipCurrentDraw()) {
            skipped_unfinished = true;
            last_graphics_entry = nullptr;
            current_graphics_pipeline = nullptr;
            return nullptr;
        }
        auto job = pit->second;
        {
            std::unique_lock lock{job->mutex};
            job->cv.wait(lock, [&job] { return job->done; });
        }
        FinishPendingGraphics(graphics_key);
        GraphicsPipelineCacheKey normalized = graphics_key;
        NormalizeGraphicsCbufSizes(normalized);
        const auto it = graphics_cache.find(normalized);
        if (it != graphics_cache.end()) {
            record_transition(&it->first, &it->second);
            current_graphics_pipeline =
                it->second && it->second->IsValid() ? it->second.get() : nullptr;
            return finish_lookup(BuiltPipeline(current_graphics_pipeline));
        }
        last_graphics_entry = nullptr;
        current_graphics_pipeline = nullptr;
        return nullptr;
    }

    if (CanSkipCurrentDraw() && !graphics_key.state.xfb_enabled &&
        QueueGraphicsPipeline(graphics_key)) {
        skipped_unfinished = true;
        last_graphics_entry = nullptr;
        current_graphics_pipeline = nullptr;
        return nullptr;
    }

    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);
    auto built = CreateGraphicsPipeline(main_pools, graphics_key, environments);
    if (built) {
        ObserveGraphicsEnvironments(graphics_key, environments);
    }
    GraphicsPipelineCacheKey normalized = graphics_key;
    NormalizeGraphicsCbufSizes(normalized);
    const auto [pair, is_new] = graphics_cache.try_emplace(normalized);
    auto& pipeline = pair->second;
    if (is_new || !pipeline) {
        pipeline = std::move(built);
        if (pipeline) {
            SerializeGraphicsPipeline(normalized, environments);
        }
    }
    record_transition(&pair->first, &pipeline);

    current_graphics_pipeline = pipeline && pipeline->IsValid() ? pipeline.get() : nullptr;
    return finish_lookup(BuiltPipeline(current_graphics_pipeline));
}

GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline) const noexcept {
    if (pipeline == nullptr || pipeline->IsBuilt()) {
        return pipeline;
    }
    if (!CanSkipCurrentDraw()) {
        return pipeline;
    }
    skipped_unfinished = true;
    // Counted, the object is missing until its pipeline is ready, so look here
    // if a title shows constant pop-in.
    static std::atomic<u64> skipped{0};
    const u64 count = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (std::has_single_bit(count)) {
        LOG_DEBUG(Render_Metal, "Skipped {} draw(s) whose pipeline was still compiling", count);
    }
    return nullptr;
}

ComputePipeline* PipelineCache::CurrentComputePipeline() {
    const VideoCommon::ShaderInfo* const shader = ComputeShader();
    if (shader == nullptr || kepler_compute == nullptr) {
        return nullptr;
    }

    // The key has to carry the launch geometry and not just the shader hash. The
    // same kernel dispatched with a different threadgroup size or shared memory
    // allocation is a different Metal pipeline.
    const auto& qmd = kepler_compute->launch_description;
    const auto compute_pool_key = [&]() -> u32 {
        const auto pool = VideoCommon::ComputeTexturePoolInfo(*kepler_compute);
        // Fold texture/sampler pool sizes and linked flag into 32 bits without changing
        // ComputePipelineCacheKey's on-disk layout or CACHE_VERSION.
        u64 h = (static_cast<u64>(pool.num_textures) << 17) |
                (static_cast<u64>(pool.linked_samplers ? 0u : pool.num_samplers) << 1) |
                (pool.linked_samplers ? 1ull : 0ull);
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebull;
        h ^= h >> 31;
        return static_cast<u32>(h ^ (h >> 32));
    };
    const u32 cbuf_mask = cbuf_size_dependencies.Mask(shader->unique_hash);
    compute_key = ComputePipelineCacheKey{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size = {qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
        .cbuf_sizes = {},
        .texture_table_size = (cbuf_mask & VideoCommon::TEXTURE_POOL_SIZE_BIT) != 0
                                  ? compute_pool_key()
                                  : std::min(kepler_compute->regs.tic.limit + 1U, 1U << 20),
    };
    for (u32 bank = 0; bank < compute_key.cbuf_sizes.size(); ++bank) {
        if ((cbuf_mask & (1U << bank)) != 0 &&
            ((qmd.const_buffer_enable_mask.Value() >> bank) & 1U) != 0) {
            compute_key.cbuf_sizes[bank] = qmd.const_buffer_config[bank].size.Value();
        }
    }

    const auto MakeLive = [&](ComputePipelineCacheKey& out_key) -> std::unique_ptr<ComputePipeline> {
        const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
        VideoCommon::ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base,
                                            qmd.program_start};
        env.SetCachedSize(shader->size_bytes);
        auto built = CreateComputePipeline(main_pools, compute_key, env);
        if (built) {
            cbuf_size_dependencies.Observe(compute_key.unique_hash, env.CbufSizeMask());
            const u32 observed_mask = cbuf_size_dependencies.Mask(compute_key.unique_hash);
            Vulkan::CbufSizeDependencies::Normalize(out_key.cbuf_sizes, observed_mask);
            if ((observed_mask & VideoCommon::TEXTURE_POOL_SIZE_BIT) != 0) {
                out_key.texture_table_size = compute_pool_key();
            }
        }
        // Recorded so the next run preloads it instead of compiling it again.
        if (built && !pipeline_cache_filename.empty()) {
            serialization_thread.QueueWork(
                [this, key = out_key, env_ = std::move(env)]() mutable {
                    VideoCommon::SerializePipeline(
                        key, std::array<const VideoCommon::GenericEnvironment*, 1>{&env_},
                        pipeline_cache_filename, CACHE_VERSION);
                    MaybeFlushMetal4Archive(/*graphics_added=*/false, /*compute_added=*/true);
                });
        }
        return built;
    };

    if (const auto it = compute_cache.find(compute_key); it != compute_cache.end()) {
        auto& pipeline = it->second;
        if (pipeline == nullptr) {
            constexpr u32 MAX_RETRIES = 3;
            u32& attempts = compute_failures[compute_key];
            if (attempts < MAX_RETRIES) {
                ++attempts;
                ComputePipelineCacheKey normalized = compute_key;
                auto built = MakeLive(normalized);
                if (built) {
                    auto& target = compute_cache[normalized];
                    if (!target) {
                        target = std::move(built);
                    }
                    return target && target->IsValid() ? target.get() : nullptr;
                }
            }
        }
        return pipeline && pipeline->IsValid() ? pipeline.get() : nullptr;
    }

    ComputePipelineCacheKey normalized = compute_key;
    auto built = MakeLive(normalized);
    auto& target = compute_cache[normalized];
    if (!target) {
        target = std::move(built);
    }
    return target && target->IsValid() ? target.get() : nullptr;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    const GraphicsEnvironments& environments, bool build_in_parallel) try {
    using Maxwell = Tegra::Engines::Maxwell3D::Regs;

    // No guest-register access from here down, and the pools are a parameter
    // because the disk cache rebuilds pipelines on several threads at once.
    pools.ReleaseContents();

    std::span<Shader::Environment* const> envs = environments.Span();

    // --- Guest bytecode -> IR --------------------------------------------
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};

    size_t env_index{0};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        if (env_index >= envs.size()) {
            // The key names a stage with no environment. Live draws build both
            // together, but an older shader cache file can land here and
            // dereferencing past the span takes the process with it.
            LOG_WARNING(Render_Metal, "Pipeline {:016x} is missing stage {}", key.Hash(), index);
            return nullptr;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);

        if (!uses_vertex_a || index != 1) {
            programs[index] = Shader::Maxwell::TranslateProgram(pools.inst, pools.block,
                                                                env, cfg, host_info);
        } else {
            auto& program_va{programs[0]};
            auto program_vb{Shader::Maxwell::TranslateProgram(pools.inst, pools.block,
                                                              env, cfg, host_info)};
            programs[index] = Shader::Maxwell::MergeDualVertexPrograms(program_va, program_vb, env);
        }
    }

    // --- IR -> SPIR-V -> MSL ---------------------------------------------

    // Maxwell program index of the geometry stage. Not the index the buffer cache
    // or our own per-stage arrays use for it.
    constexpr size_t GEOMETRY_PROGRAM = 4;

    // Without host geometry shaders, a geometry shader that only forwards a
    // triangle and copies a generic attribute into gl_Layer (e.g. UE4's
    // FWriteToSliceGS for 3D LUTs / volumetric passes) is folded directly into
    // the vertex shader via [[render_target_array_index]].
    std::optional<Shader::IR::Attribute> forwarded_layer;
    if (key.unique_hashes[GEOMETRY_PROGRAM] != 0 && key.unique_hashes[2] == 0 &&
        key.unique_hashes[3] == 0 && uses_vertex_b) {
        const auto input_topology = MaxwellToInputTopology(
            static_cast<Maxwell::PrimitiveTopology>(key.state.topology));
        const u32 input_vertices = Shader::InputTopologyVertices::vertices(input_topology);
        forwarded_layer = Shader::Optimization::FindForwardedLayerAttribute(
            programs[GEOMETRY_PROGRAM], input_vertices);
        if (forwarded_layer) {
            programs[1].info.stores.Set(Shader::IR::Attribute::Layer);
            LOG_INFO(Render_Metal,
                     "Folding layer-forwarding geometry shader {:016x} into vertex shader {:016x}",
                     key.unique_hashes[GEOMETRY_PROGRAM], key.unique_hashes[1]);
        }
    }

    // Metal has no stream-out stage so transform feedback rides the capture
    // kernel. The guest vertex program runs as compute and a gather kernel moves
    // each varying into the guest's buffers, while the draw still renders
    // through the real vertex function.
    //
    // The XFB layout is read from the registers and not the key, so maxwell3d
    // has to be non-null. It isn't during the disk-cache preload, which is why
    // we never serialize preloaded xfb pipelines (see CurrentGraphicsPipeline).
    const bool want_xfb_capture = key.state.xfb_enabled && maxwell3d != nullptr;

    // Read from the registers here and handed to GeometryEmulation below, the
    // gather kernel needs them to place each varying in the guest's layout.
    std::array<Shader::TransformFeedbackVarying, 256> xfb_varyings{};
    u32 xfb_varying_count = 0;
    if (want_xfb_capture) {
        VideoCommon::TransformFeedbackState xfb_state{};
        const auto& regs = maxwell3d->regs;
        for (size_t i = 0; i < xfb_state.layouts.size(); ++i) {
            const auto& control = regs.transform_feedback.controls[i];
            xfb_state.layouts[i] = {
                .stream = control.stream,
                .varying_count = control.varying_count,
                .stride = control.stride,
            };
        }
        xfb_state.varyings = regs.stream_out_layout;
        std::tie(xfb_varyings, xfb_varying_count) =
            VideoCommon::MakeTransformFeedbackVaryings(xfb_state);
    }

    std::array<const Shader::Info*, NUM_PIPELINE_STAGES> infos{};
    std::array<MslShader, NUM_PIPELINE_STAGES> shaders{};
    std::array<const MslShader*, NUM_PIPELINE_STAGES> shader_ptrs{};

    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;
    const BindingMap binding_map{};

    // IR-level VTG compute emulation (shared with Vulkan/MoltenVK): runs guest
    // vertex and geometry stages as compute kernels and feeds emitted primitives
    // to a generated passthrough vertex shader.
    std::unique_ptr<VtgEmulation> vtg_emulation;
    Shader::IR::Program vtg_passthrough;
    Shader::Info vtg_vertex_guest_info;
    Shader::Info vtg_geometry_guest_info;
    const std::optional<Shader::VtgTopology> vtg_topology =
        MaxwellToVtgTopology(static_cast<Maxwell::PrimitiveTopology>(key.state.topology));
    if (Metal::Options::GeometryEmulation() && !forwarded_layer && vtg_topology &&
        key.unique_hashes[GEOMETRY_PROGRAM] != 0 && key.unique_hashes[2] == 0 &&
        key.unique_hashes[3] == 0 && uses_vertex_b) {
        Shader::IR::Program& vertex = programs[1];
        Shader::IR::Program& geometry = programs[GEOMETRY_PROGRAM];
        const auto vertex_runtime =
            MakeRuntimeInfo(key.state, vertex, nullptr, /*has_geometry=*/true);
        Shader::Maxwell::ConvertLegacyToGeneric(vertex, vertex_runtime);
        auto geometry_runtime =
            MakeRuntimeInfo(key.state, geometry, &vertex, /*has_geometry=*/true);
        // Clip-space depth conversion and Y-negate happen in the generated
        // passthrough vertex stage, not inside the compute geometry kernel.
        geometry_runtime.convert_depth_mode = false;
        geometry_runtime.y_negate = false;
        Shader::Maxwell::ConvertLegacyToGeneric(geometry, geometry_runtime);

        const Shader::VaryingState geometry_stores = geometry.info.stores;
        const Shader::VtgVaryingLayout vertex_layout{vertex.info.stores};
        const Shader::VtgVaryingLayout geometry_layout{geometry_stores};
        const u32 input_vertices =
            Shader::InputTopologyVertices::vertices(geometry_runtime.input_topology);
        const Shader::OutputTopology geometry_topology = geometry.output_topology;

        auto vtg = std::make_unique<VtgEmulation>();
        if (Shader::Optimization::VtgVertexToCompute(vertex, MakeVtgVertexInputs(key.state),
                                                     vertex_layout, vtg->vertex_bindings)) {
            if (Shader::Optimization::VtgGeometryToCompute(geometry, *vtg_topology, input_vertices,
                                                           vertex_layout, geometry_layout,
                                                           vtg->geometry_bindings)) {
                Shader::Backend::Bindings vertex_compute_binding{};
                const std::vector<u32> vertex_spv = Shader::Backend::SPIRV::EmitSPIRV(
                    graphics_profile, vertex_runtime, vertex, vertex_compute_binding);
                vtg->vertex_kernel = RunOnLargeStack([&]() {
                    return TranslateSpirvToMsl(device, vertex_spv, Shader::Stage::Compute,
                                               vertex.info, binding_map, false);
                });

                Shader::Backend::Bindings geometry_compute_binding{};
                const std::vector<u32> geometry_spv = Shader::Backend::SPIRV::EmitSPIRV(
                    graphics_profile, geometry_runtime, geometry, geometry_compute_binding);
                vtg->geometry_kernel = RunOnLargeStack([&]() {
                    return TranslateSpirvToMsl(device, geometry_spv, Shader::Stage::Compute,
                                               geometry.info, binding_map, false);
                });

                vtg_vertex_guest_info = vertex.info;
                vtg_vertex_guest_info.storage_buffers_descriptors.resize(
                    vtg->vertex_bindings.base);
                vtg_geometry_guest_info = geometry.info;
                vtg_geometry_guest_info.storage_buffers_descriptors.resize(
                    vtg->geometry_bindings.base);

                vtg->topology = *vtg_topology;
                vtg->vertex_stride = vertex_layout.stride;
                vtg->geometry_stride = geometry_layout.stride;
                vtg->uses_constant_attributes = key.state.uses_constant_attributes;
                vtg->constant_binding = key.state.constant_binding;
                if (want_xfb_capture) {
                    vtg->xfb_enabled = true;
                    vtg->geometry_layout = geometry_layout;
                }

                vtg_passthrough = Shader::Maxwell::GenerateVtgPassthroughVertex(
                    pools.inst, pools.block, geometry, geometry_stores, geometry_layout);
                auto passthrough_runtime =
                    MakeRuntimeInfo(key.state, vtg_passthrough, nullptr, /*has_geometry=*/false);
                passthrough_runtime.convert_depth_mode = key.state.ndc_minus_one_to_one;
                passthrough_runtime.y_negate = key.state.y_direction_negate;
                if (geometry_topology == Shader::OutputTopology::PointList) {
                    passthrough_runtime.fixed_state_point_size =
                        std::bit_cast<float>(key.state.point_size);
                }
                const std::vector<u32> passthrough_spv = Shader::Backend::SPIRV::EmitSPIRV(
                    graphics_profile, passthrough_runtime, vtg_passthrough, binding);
                const bool rasterizes_points =
                    geometry_topology == Shader::OutputTopology::PointList;
                constexpr size_t VERTEX_SLOT = static_cast<size_t>(PipelineStage::Vertex);
                constexpr size_t GEOMETRY_SLOT = static_cast<size_t>(PipelineStage::Geometry);
                shaders[VERTEX_SLOT] = RunOnLargeStack([&]() {
                    return TranslateSpirvToMsl(device, passthrough_spv, Shader::Stage::VertexB,
                                               vtg_passthrough.info, binding_map,
                                               rasterizes_points);
                });
                shader_ptrs[VERTEX_SLOT] = &shaders[VERTEX_SLOT];
                infos[VERTEX_SLOT] = &vtg_vertex_guest_info;
                infos[GEOMETRY_SLOT] = &vtg_geometry_guest_info;
                previous_stage = &vtg_passthrough;
                vtg_emulation = std::move(vtg);
                if (vtg_emulation->xfb_enabled) {
                    LOG_INFO(Render_Metal,
                             "Running vertex shader {:016x} and geometry shader {:016x} as "
                             "compute (with XFB)",
                             key.unique_hashes[1], key.unique_hashes[GEOMETRY_PROGRAM]);
                } else {
                    LOG_INFO(Render_Metal,
                             "Running vertex shader {:016x} and geometry shader {:016x} as compute",
                             key.unique_hashes[1], key.unique_hashes[GEOMETRY_PROGRAM]);
                }
            } else {
                LOG_WARNING(Render_Metal, "Geometry shader {:016x} cannot run as compute",
                            key.unique_hashes[GEOMETRY_PROGRAM]);
            }
        }
    }

    // Filled while walking the stages and used once they've all been seen. The
    // capture kernel comes from the vertex program (emitted first) and the
    // passthrough's varyings from the fragment program (emitted last).
    // When VTG compute emulation handles both VS+GS and XFB, no separate VS capture kernel is
    // needed.
    const bool want_vs_xfb_capture = want_xfb_capture && !vtg_emulation;
    auto geometry_emulation = want_vs_xfb_capture ? std::make_unique<GeometryEmulation>()
                                                  : std::unique_ptr<GeometryEmulation>{};
    bool dropped_geometry = false;

    // Pixel local reads of a color attachment come from tile memory through framebuffer
    // fetch, so the draw needs no pass break. Not under geometry emulation or xfb capture:
    // the stage feeding the fragment shader is no longer the guest one.
    const bool reads_color_attachments = !vtg_emulation && !want_xfb_capture;
    u32 screen_position_generics{};

    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        // VTG compute lowering already translated Vertex and Geometry (mutating
        // their stage to Compute), so only the Fragment stage remains.
        if (vtg_emulation && programs[index].stage != Shader::Stage::Fragment) {
            continue;
        }
        Shader::IR::Program& program{programs[index]};

        // A Metal render pipeline has only a vertex and a fragment function.
        // Tessellation and geometry use a completely different execution model,
        // so skip them instead of emitting something we can't bind.
        size_t slot{};
        if (program.stage == Shader::Stage::VertexA || program.stage == Shader::Stage::VertexB) {
            if (vtg_emulation) {
                continue;
            }
            slot = static_cast<size_t>(PipelineStage::Vertex);
        } else if (program.stage == Shader::Stage::Fragment) {
            slot = static_cast<size_t>(PipelineStage::Fragment);
        } else if (program.stage == Shader::Stage::Geometry) {
            if (forwarded_layer || vtg_emulation) {
                continue;
            }
            // The primitives this stage would have emitted are just gone, and
            // the vertex stream hits the rasteriser in the form the geometry
            // shader was meant to eat.
            LOG_WARNING(Render_Metal,
                        "Dropping geometry shader: passthrough={}, output_topology={}, "
                        "invocations={}",
                        program.is_geometry_passthrough,
                        static_cast<u32>(program.output_topology), program.invocations);
            dropped_geometry = true;
            continue;
        } else if (program.stage == Shader::Stage::TessellationControl ||
                   program.stage == Shader::Stage::TessellationEval) {
            // Dropped. The patch topology falls through PrimitiveTopology() to a
            // plain triangle list, so control points get rasterised undivided,
            // and unprojected too when the evaluation program was the stage that
            // would have projected them.
            //
            // Metal does have a tessellator so this is a gap and not an
            // impossibility, the domain state already reaches the recompiler.
            static std::atomic<bool> reported{false};
            if (!reported.exchange(true)) {
                LOG_WARNING(Render_Metal,
                            "Dropping tessellation: domain={} spacing={} clockwise={} "
                            "control_points={}. The patch control points are rasterised "
                            "undivided.",
                            key.state.tessellation_domain, key.state.tessellation_spacing,
                            key.state.tessellation_clockwise, key.state.patch_control_points);
            }
            continue;
        } else {
            LOG_WARNING(Render_Metal, "Skipping unsupported shader stage {}",
                        static_cast<u32>(program.stage));
            continue;
        }

        // The xfb varyings are NOT passed into the translation on purpose. Their
        // decorations make SPIRV-Cross emit component-split outputs whose
        // capture stores need an address-space cast MSL rejects, so the kernel
        // fails to build and every capture comes back zeros.
        const auto runtime_info{
            MakeRuntimeInfo(key.state, program, previous_stage, /*has_geometry=*/false,
                            forwarded_layer)};
        Shader::Maxwell::ConvertLegacyToGeneric(program, runtime_info);
        if (reads_color_attachments) {
            if (program.stage == Shader::Stage::Fragment) {
                FindColorAttachmentReads(program, key.state, screen_position_generics);
            } else {
                screen_position_generics =
                    Shader::Optimization::FindScreenPositionGenerics(program);
            }
        }

        if (program.stage == Shader::Stage::Fragment &&
            program.info.loads[Shader::IR::Attribute::PositionY]) {
            program.info.uses_render_area = true;
        }

        const std::vector<u32> code{
            Shader::Backend::SPIRV::EmitSPIRV(graphics_profile, runtime_info, program, binding)};

        // SPIRV-Cross recurses deeply enough to overflow a worker thread's
        // default 512 KiB stack on macOS, so the translation hops to a pthread
        // with SPIRV_CROSS_STACK_SIZE.
        const Shader::Stage stage = program.stage;
        const Shader::Info& shader_info = program.info;

        if (want_vs_xfb_capture && slot == static_cast<size_t>(PipelineStage::Vertex)) {
            // For XFB the capture kernel is an addition, so fall through and
            // translate the vertex program normally as well.
            geometry_emulation->capture = RunOnLargeStack([&]() {
                return TranslateVertexToCaptureKernel(device, code, shader_info, binding_map,
                                                      key.state);
            });
        }

        const bool rasterizes_points =
            key.state.topology_class == MTL::PrimitiveTopologyClassPoint;
        shaders[slot] = RunOnLargeStack([&]() {
            return TranslateSpirvToMsl(device, code, stage, shader_info, binding_map,
                                       rasterizes_points, key.state.d24_frag_depth_equal);
        });
        infos[slot] = &program.info;
        shader_ptrs[slot] = &shaders[slot];

        previous_stage = &program;
    }

    if (shader_ptrs[static_cast<size_t>(PipelineStage::Vertex)] == nullptr) {
        LOG_WARNING(Render_Metal, "Pipeline {:016x} has no vertex stage", key.Hash());
        return nullptr;
    }

    if (want_vs_xfb_capture && geometry_emulation != nullptr) {
        constexpr size_t VERTEX_SLOT = static_cast<size_t>(PipelineStage::Vertex);
        const Shader::Info* const vs_info = infos[VERTEX_SLOT];
        if (geometry_emulation->capture.source.empty() || vs_info == nullptr) {
            // The pipeline still renders, the guest's feedback buffers just stay
            // unwritten.
            LOG_WARNING(Render_Metal, "Pipeline {:016x} wanted XFB capture but has no kernel",
                        key.Hash());
            geometry_emulation.reset();
        } else {
            geometry_emulation->xfb_only = true;

            // Capture buffer layout, stored generics in ascending location order
            // then gl_Position last. Has to match what SPIRV-Cross emits for
            // capture_output_to_buffer, see SetupGeometryAsCompute.
            geometry_emulation->xfb_slot_of_generic.fill(-1);
            s8 next = 0;
            for (u32 location = 0; location < 32; ++location) {
                if (vs_info->stores.Generic(location)) {
                    geometry_emulation->xfb_slot_of_generic[location] = next++;
                }
            }
            geometry_emulation->xfb_position_slot = next;

            // Measure the stride off the generated struct, never off the stores
            // mask. SPIRV-Cross appends members (gl_PointSize) the program never
            // stored, and a wrong stride drifts every vertex after the first.
            {
                u32 struct_bytes = 0;
                const std::string& src = geometry_emulation->capture.source;
                const size_t begin = src.find("struct main0_out");
                const size_t end = begin == std::string::npos ? std::string::npos
                                                              : src.find("};", begin);
                if (begin != std::string::npos && end != std::string::npos) {
                    size_t cursor = begin;
                    while (cursor < end) {
                        const size_t line_end = src.find('\n', cursor);
                        const std::string_view line{src.data() + cursor,
                                                    std::min(line_end, end) - cursor};
                        if (line.find("float4 ") != std::string_view::npos) {
                            struct_bytes += 16;
                        } else if (line.find("float3 ") != std::string_view::npos) {
                            struct_bytes += 12;
                        } else if (line.find("float2 ") != std::string_view::npos) {
                            struct_bytes += 8;
                        } else if (line.find("float ") != std::string_view::npos ||
                                   line.find("uint ") != std::string_view::npos) {
                            struct_bytes += 4;
                        }
                        cursor = line_end == std::string::npos ? end : line_end + 1;
                    }
                }
                // A location split into partial members (float2 halves and the
                // like) breaks the one-float4-per-location slot table. Capture
                // nothing instead of scattering from wrong offsets.
                const bool split_members =
                    begin != std::string::npos && end != std::string::npos &&
                    (src.find("float3 ", begin) < end || src.find("float2 ", begin) < end);
                if (struct_bytes == 0 || split_members) {
                    LOG_WARNING(Render_Metal,
                                "Pipeline {:016x} XFB capture skipped: capture struct is {} ",
                                key.Hash(), struct_bytes == 0 ? "unmeasurable" : "split");
                    geometry_emulation.reset();
                } else {
                    geometry_emulation->input_stride = Common::AlignUp(struct_bytes, 16) / 16;
                }
            }

            // The guest's capture table, from the registers. This key doesn't
            // carry XFB state.
            if (geometry_emulation != nullptr) {
            const auto& varyings = xfb_varyings;
            const u32 count = xfb_varying_count;
            geometry_emulation->xfb_varyings = varyings;
            geometry_emulation->xfb_varying_count = count;

            LOG_INFO(Render_Metal,
                     "Pipeline {:016x} captures transform feedback: {} varyings, capture stride "
                     "{} float4s",
                     key.Hash(), count, geometry_emulation->input_stride);

            if (geometry_emulation->capture.argument_buffer_slots !=
                shaders[VERTEX_SLOT].argument_buffer_slots) {
                // The capture kernel is fed the render vertex stage's argument
                // buffer, so a layout disagreement makes every resource the
                // kernel reads come from the wrong slot.
                LOG_WARNING(Render_Metal,
                            "Pipeline {:016x} XFB capture argument layout drift", key.Hash());
            }

            }
        }
    }

    // The MSL compile runs on the worker pool and we return the pipeline right
    // away, the draw path waits for the build on the scheduler thread. A null
    // worker compiles inline, which is what the disk loader wants. It's already
    // on a worker, and re-queuing would have it report every pipeline as built
    // the moment it was queued.
    auto pipeline = std::make_unique<GraphicsPipeline>(
        device, scheduler, build_in_parallel ? workers.get() : nullptr, key.state, infos,
        shader_ptrs, &shader_notify, std::move(geometry_emulation), std::move(vtg_emulation));
    if (dropped_geometry) {
        pipeline->SetDroppedGeometry();
    }
    LOG_DEBUG(Render_Metal, "Queued graphics pipeline {:016x}", key.Hash());
    return pipeline;
} catch (const std::exception& error) {
    LOG_ERROR(Render_Metal, "Failed to build a graphics pipeline: {}", error.what());
    return nullptr;
}

std::unique_ptr<ComputePipeline> PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    bool build_in_parallel) try {
    pools.ReleaseContents();

    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};
    auto program{Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};

    // A kernel asking for more threadgroup memory than the device has can't be
    // dispatched at all. First try packing 16-byte-strided records down to 12
    // bytes (same workaround as Vulkan/MoltenVK); only clamp if still over.
    const u32 max_shared = static_cast<u32>(device.GetMaxThreadgroupMemoryLength());
    if (program.shared_memory_size > max_shared) {
        const u32 requested = program.shared_memory_size;
        if (Shader::Optimization::TryPackSharedMemory16To12(program, max_shared)) {
            LOG_INFO(Render_Metal,
                     "Compute kernel {:#016x}: compacted threadgroup memory from {} to {} bytes",
                     key.unique_hash, requested, program.shared_memory_size);
        }
        if (program.shared_memory_size > max_shared) {
            LOG_WARNING(Render_Metal,
                        "Compute kernel {:#016x} wants {} KiB of threadgroup memory, device allows "
                        "{} KiB - clamping",
                        key.unique_hash, program.shared_memory_size / 1024, max_shared / 1024);
            program.shared_memory_size = max_shared;
        }
    }

    // Enable per-4KiB-page compute storage write tracking (only for real compute shaders,
    // never VTG vertex/geometry kernels) so TrackedReadback copies and retires only pages
    // the kernel actually wrote.
    Shader::Profile compute_profile = profile;
    compute_profile.track_storage_writes = true;
    std::vector<u32> code{Shader::Backend::SPIRV::EmitSPIRV(compute_profile, program)};

    auto pipeline = std::make_unique<ComputePipeline>(
        device, scheduler, build_in_parallel ? workers.get() : nullptr, program.info,
        program.workgroup_size, std::move(code), &shader_notify);
    if (!pipeline->IsValid()) {
        return nullptr;
    }
    LOG_DEBUG(Render_Metal, "Queued compute pipeline {:#016x}", key.unique_hash);
    return pipeline;
} catch (const std::exception& error) {
    // Report what the decoder was looking at. "Invalid insn 0x0" means we read
    // memory holding nothing, not an unimplemented opcode.
    const GPUVAddr base = kepler_compute != nullptr ? kepler_compute->regs.code_loc.Address() : 0;
    const u32 start = kepler_compute != nullptr ? kepler_compute->launch_description.program_start
                                                : 0;
    u64 first_words = 0;
    bool mapped = false;
    u64 cpu_addr = 0;
    if (gpu_memory != nullptr) {
        first_words = gpu_memory->Read<u64>(base + start);
        // An unmapped read and a mapped-but-zero read both come back zero but
        // mean opposite things, so report `mapped` separately.
        const auto translated = gpu_memory->GpuToCpuAddress(base + start);
        mapped = translated.has_value();
        cpu_addr = mapped ? *translated : 0;
    }
    LOG_ERROR(Render_Metal,
              "Failed to build compute pipeline {:#016x} at gpu {:#x}+{:#x} (mapped={} cpu {:#x}, "
              "first words {:#018x}): {}",
              key.unique_hash, base, start, mapped, cpu_addr, first_words, error.what());
    return nullptr;
}

} // namespace Metal
