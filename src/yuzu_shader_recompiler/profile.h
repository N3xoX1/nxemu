// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_common/common_types.h"

namespace Shader {

/// Bits of a texel buffer view's element count that hold its leading padding, see
/// Profile::texel_buffer_pad_in_size.
constexpr u32 TEXEL_BUFFER_PAD_BITS = 4;
constexpr u32 TEXEL_BUFFER_PAD_MASK = (1U << TEXEL_BUFFER_PAD_BITS) - 1;
/// Bytes a host buffer keeps readable past its data so that any texel buffer view inside the data
/// can be rounded up to a whole number of 1 << TEXEL_BUFFER_PAD_BITS elements of up to 16 bytes.
constexpr u32 TEXEL_BUFFER_TAIL_BYTES = TEXEL_BUFFER_PAD_MASK * 16;

struct Profile {
    u32 supported_spirv{0x00010000};
    bool unified_descriptor_binding{};
    bool support_descriptor_aliasing{};
    bool support_sampled_image_array_nonuniform_indexing{};
    bool support_uniform_texel_buffer_array_nonuniform_indexing{};
    bool support_storage_image_array_nonuniform_indexing{};
    // MoltenVK needs one canonical uniform view; storage-buffer aliases remain available.
    bool disable_uniform_buffer_descriptor_aliasing{};
    bool support_int8{};
    bool support_int16{};
    bool support_int64{};
    bool support_vertex_instance_id{};
    bool support_float_controls{};
    bool support_separate_denorm_behavior{};
    bool support_separate_rounding_mode{};
    bool support_fp16_denorm_preserve{};
    bool support_fp32_denorm_preserve{};
    bool support_fp16_denorm_flush{};
    bool support_fp32_denorm_flush{};
    bool support_fp16_signed_zero_nan_preserve{};
    bool support_fp32_signed_zero_nan_preserve{};
    bool support_fp64_signed_zero_nan_preserve{};
    bool support_explicit_workgroup_layout{};
    bool support_vote{};
    /// Subgroup built-ins and operations are usable before the fragment stage
    bool support_subgroup_in_vertex_stages{true};
    bool support_viewport_index_layer_non_geometry{};
    bool support_viewport_mask{};
    bool support_typeless_image_loads{};
    bool support_demote_to_helper_invocation{};
    bool support_int64_atomics{};
    bool support_derivative_control{};
    bool support_geometry_shader_passthrough{};
    bool support_native_ndc{};
    bool support_gl_nv_gpu_shader_5{};
    bool support_gl_amd_gpu_shader_half_float{};
    bool support_gl_texture_shadow_lod{};
    bool support_gl_warp_intrinsics{};
    bool support_gl_variable_aoffi{};
    bool support_gl_sparse_textures{};
    bool support_gl_derivative_control{};
    bool support_scaled_attributes{};
    bool support_multi_viewport{};
    bool support_geometry_streams{};

    bool warp_size_potentially_larger_than_guest{};

    bool lower_left_origin_mode{};
    /// Fragment outputs have to be declared even if they are not written to avoid undefined values.
    /// See Ori and the Blind Forest's main menu for reference.
    bool need_declared_frag_colors{};
    /// The host rounds values halfway between two 8-bit normalized steps up instead of down
    bool has_unorm8_round_half_up{};
    /// Prevents fast math optimizations that may cause inaccuracies
    bool need_fastmath_off{};
    /// Some GPU vendors use a different rounding precision when calculating texture pixel
    /// coordinates with the 16.8 format in the ImageGather instruction than the Maxwell
    /// architecture. Applying an offset does fix this mismatching rounding behaviour.
    bool need_gather_subpixel_offset{};

    /// OpFClamp is broken and OpFMax + OpFMin should be used instead
    bool has_broken_spirv_clamp{};
    /// The Position builtin needs to be wrapped in a struct when used as an input
    bool has_broken_spirv_position_input{};
    /// Offset image operands with an unsigned type do not work
    bool has_broken_unsigned_image_offsets{};
    /// Signed instructions with unsigned data types are misinterpreted
    bool has_broken_signed_operations{};
    /// Float controls break when fp16 is enabled
    bool has_broken_fp16_float_controls{};
    /// Dynamic vec4 indexing is broken on some OpenGL drivers
    bool has_gl_component_indexing_bug{};
    /// The precise type qualifier is broken in the fragment stage of some drivers
    bool has_gl_precise_bug{};
    /// Some drivers do not properly support floatBitsToUint when used on cbufs
    bool has_gl_cbuf_ftou_bug{};
    /// Some drivers poorly optimize boolean variable references
    bool has_gl_bool_ref_bug{};
    /// NoContraction makes fragment and compute shaders much slower to build
    bool has_slow_fragment_no_contraction{};
    /// Ignores SPIR-V ordered vs unordered using GLSL semantics
    bool ignore_nan_fp_comparisons{};
    /// Some drivers have broken support for OpVectorExtractDynamic on subgroup mask inputs
    bool has_broken_spirv_subgroup_mask_vector_extract_dynamic{};

    u32 gl_max_compute_smem_size{};

    /// Maxwell and earlier nVidia architectures have broken robust support
    bool has_broken_robust{};

    u64 min_ssbo_alignment{};

    u32 max_user_clip_distances{};

    /// Largest compute shared memory the host accepts, in bytes. Zero means no limit.
    u32 max_compute_shared_memory_size{};

    /// The host cannot start a texel buffer view at an arbitrary offset: it rounds it down to
    /// its alignment. Views are then created at the aligned offset and their element count
    /// carries the number of leading elements to skip in its low TEXEL_BUFFER_PAD_BITS bits.
    bool texel_buffer_pad_in_size{};

    bool SupportsUniformBufferDescriptorAliasing() const {
        return support_descriptor_aliasing && !disable_uniform_buffer_descriptor_aliasing;
    }
};

} // namespace Shader
