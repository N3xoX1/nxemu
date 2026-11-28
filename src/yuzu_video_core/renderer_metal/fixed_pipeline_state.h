// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <type_traits>

#include "yuzu_common/bit_field.h"
#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/surface.h"

namespace Metal {

constexpr size_t MAX_VERTEX_ATTRIBUTES = 32;
constexpr size_t MAX_VERTEX_BUFFERS = 32;
constexpr size_t MAX_COLOR_ATTACHMENTS = 8;

/// A Metal enum stored in `Storage`: metal-cpp's enums are 8 bytes wide, and the pipeline
/// key is copied, compared and hashed bytewise on every draw.
template <typename Enum, typename Storage>
struct PackedEnum {
    Storage raw{};

    constexpr PackedEnum() = default;
    constexpr PackedEnum(Enum value) noexcept : raw{static_cast<Storage>(value)} {}

    constexpr operator Enum() const noexcept {
        return static_cast<Enum>(raw);
    }

    [[nodiscard]] constexpr bool operator==(const PackedEnum&) const noexcept = default;
    [[nodiscard]] constexpr bool operator==(Enum value) const noexcept {
        return raw == static_cast<Storage>(value);
    }
};

#pragma pack(push, 1)
struct FixedPipelineState {
    struct VertexAttribute {
        u32 buffer_index{};
        u32 offset{};
        PackedEnum<MTL::VertexFormat, u8> format{MTL::VertexFormatInvalid};

        [[nodiscard]] bool operator==(const VertexAttribute&) const noexcept = default;
    };

    struct VertexBinding {
        u32 stride{};
        u32 divisor{};
        bool instanced{};
        bool enabled{};

        [[nodiscard]] bool operator==(const VertexBinding&) const noexcept = default;
    };

    struct ColorAttachment {
        PackedEnum<MTL::PixelFormat, u16> format{MTL::PixelFormatInvalid};
        PackedEnum<MTL::ColorWriteMask, u8> write_mask{MTL::ColorWriteMaskAll};

        bool blend_enable{};
        PackedEnum<MTL::BlendFactor, u8> src_rgb{MTL::BlendFactorOne};
        PackedEnum<MTL::BlendFactor, u8> dst_rgb{MTL::BlendFactorZero};
        PackedEnum<MTL::BlendOperation, u8> rgb_op{MTL::BlendOperationAdd};
        PackedEnum<MTL::BlendFactor, u8> src_alpha{MTL::BlendFactorOne};
        PackedEnum<MTL::BlendFactor, u8> dst_alpha{MTL::BlendFactorZero};
        PackedEnum<MTL::BlendOperation, u8> alpha_op{MTL::BlendOperationAdd};

        [[nodiscard]] bool operator==(const ColorAttachment&) const noexcept = default;
    };

    std::array<VertexAttribute, MAX_VERTEX_ATTRIBUTES> attributes{};
    std::array<VertexBinding, MAX_VERTEX_BUFFERS> bindings{};
    std::array<ColorAttachment, MAX_COLOR_ATTACHMENTS> color_attachments{};

    u32 num_color_attachments{};
    u32 sample_count{1};
    u32 alpha_test_func{};
    u32 alpha_test_ref{};
    u8 dual_source_blend{};
    bool guest_depth_d24{};
    bool d24_frag_depth_equal{};
    bool depth_equal_readonly{};
    u32 point_size{};
    std::array<u8, MAX_VERTEX_ATTRIBUTES> attribute_types{};
    std::array<u8, MAX_COLOR_ATTACHMENTS> color_output_types{};

    PackedEnum<MTL::PrimitiveTopologyClass, u8> topology_class{MTL::PrimitiveTopologyClassTriangle};
    u8 topology{};

    u8 constant_binding{};

    bool alpha_to_coverage{};
    bool alpha_to_one{};
    bool rasterizer_discard{};
    bool uses_constant_attributes{};
    bool alpha_to_coverage_dither{};
    bool y_direction_negate{};
    bool force_early_z{};
    bool ndc_minus_one_to_one{};
    bool xfb_enabled{};
    u8 tessellation_domain{};
    u8 tessellation_spacing{};
    u8 tessellation_clockwise{};
    u8 patch_control_points{};

    /// Single-sample targets cannot represent the 2x2 alpha-to-coverage dither: the
    /// fragment shader emulates it with IGN dithering and host alpha-to-coverage is off.
    [[nodiscard]] bool EmulatesAlphaToCoverageDither() const noexcept {
        return alpha_to_coverage && alpha_to_coverage_dither && sample_count <= 1;
    }

    [[nodiscard]] bool operator==(const FixedPipelineState&) const noexcept = default;

    [[nodiscard]] size_t Hash() const noexcept;
};
#pragma pack(pop)

static_assert(sizeof(FixedPipelineState) < 800, "Keep the pipeline key small, it is compared per draw");
static_assert(std::has_unique_object_representations_v<FixedPipelineState>,
              "FixedPipelineState must be flat so it can be hashed and compared bytewise");

struct DepthStencilState {
    struct StencilFace {
        MTL::CompareFunction compare{MTL::CompareFunctionAlways};
        MTL::StencilOperation fail{MTL::StencilOperationKeep};
        MTL::StencilOperation depth_fail{MTL::StencilOperationKeep};
        MTL::StencilOperation pass{MTL::StencilOperationKeep};
        u32 read_mask{0xFFFFFFFF};
        u32 write_mask{0xFFFFFFFF};

        [[nodiscard]] bool operator==(const StencilFace&) const noexcept = default;
    };

    MTL::CompareFunction depth_compare{MTL::CompareFunctionAlways};
    bool depth_write{};
    bool stencil_enable{};
    StencilFace front{};
    StencilFace back{};

    [[nodiscard]] bool operator==(const DepthStencilState&) const noexcept = default;

    [[nodiscard]] size_t Hash() const noexcept;
};

} // namespace Metal

namespace std {

template <>
struct hash<Metal::FixedPipelineState> {
    size_t operator()(const Metal::FixedPipelineState& state) const noexcept {
        return state.Hash();
    }
};

template <>
struct hash<Metal::DepthStencilState> {
    size_t operator()(const Metal::DepthStencilState& state) const noexcept {
        return state.Hash();
    }
};

} // namespace std
