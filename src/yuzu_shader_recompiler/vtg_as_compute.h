// SPDX-FileCopyrightText: Copyright 2026 NxEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>

#include <boost/container/static_vector.hpp>

#include "yuzu_common/common_types.h"
#include "yuzu_shader_recompiler/frontend/ir/attribute.h"
#include "yuzu_shader_recompiler/program_header.h"
#include "yuzu_shader_recompiler/varying_state.h"

// On hosts without geometry shaders, the vertex and geometry stages of a draw run as compute
// shaders. The vertex stage pulls its attributes from the vertex buffers and writes its outputs to
// a storage buffer, the geometry stage reads them back and writes the vertices it emits with a
// strip index buffer, and a generated vertex shader feeds the result to the rasterizer.

namespace Shader {

/// Invocations per workgroup of the stages running as compute
inline constexpr u32 VTG_LOCAL_SIZE = 32;

/// Index marking the end of a strip in the generated index buffer
inline constexpr u32 VTG_RESTART_INDEX = 0xffffffff;

/// Words of the buffer describing the draw to the stages running as compute
namespace VtgDrawInfo {
inline constexpr u32 VERTEX_COUNT = 0;
inline constexpr u32 INSTANCE_COUNT = 1;
inline constexpr u32 BASE_VERTEX = 2;
inline constexpr u32 BASE_INSTANCE = 3;
/// Bytes per index, zero on non indexed draws
inline constexpr u32 INDEX_SIZE = 4;
/// Byte offset of the first index inside the bound index buffer
inline constexpr u32 INDEX_OFFSET = 5;
inline constexpr u32 PRIMITIVE_COUNT = 6;
inline constexpr u32 INDEX_MASK = 7;
/// First word of the four words describing each vertex attribute
inline constexpr u32 ATTRIBUTES = 8;
inline constexpr u32 ATTRIBUTE_OFFSET = 0;
inline constexpr u32 ATTRIBUTE_STRIDE = 1;
inline constexpr u32 ATTRIBUTE_INSTANCED = 2;
inline constexpr u32 ATTRIBUTE_DIVISOR = 3;
inline constexpr u32 ATTRIBUTE_WORDS = 4;
inline constexpr u32 NUM_ATTRIBUTES = 32;
inline constexpr u32 NUM_WORDS = ATTRIBUTES + NUM_ATTRIBUTES * ATTRIBUTE_WORDS;
} // namespace VtgDrawInfo

enum class VtgComponentType : u8 {
    Disabled,
    Float,
    UNorm,
    SNorm,
    UInt,
    SInt,
    UScaled,
    SScaled,
};

struct VtgVertexAttribute {
    VtgComponentType type{VtgComponentType::Disabled};
    /// Vertex buffer the attribute is read from
    u8 buffer{};
    u8 components{};
    /// Bits per component, zero when the components are packed in a single word
    u8 bits{};
    /// Bit position and size of each component of a packed format
    std::array<u8, 4> packed_offsets{};
    std::array<u8, 4> packed_bits{};
};

using VtgVertexInputs = std::array<VtgVertexAttribute, VtgDrawInfo::NUM_ATTRIBUTES>;

/// Primitives assembled from the vertices produced by the vertex stage
enum class VtgTopology : u8 {
    Points,
    Lines,
    LineLoop,
    LineStrip,
    Triangles,
    TriangleStrip,
    TriangleFan,
    Quads,
    QuadStrip,
    LinesAdjacency,
    LineStripAdjacency,
    TrianglesAdjacency,
    TriangleStripAdjacency,
};

/// Number of primitives the geometry stage receives from a number of vertices
[[nodiscard]] constexpr u32 VtgPrimitiveCount(VtgTopology topology, u32 count) noexcept {
    switch (topology) {
    case VtgTopology::Points:
        return count;
    case VtgTopology::Lines:
        return count / 2;
    case VtgTopology::LineLoop:
        return count >= 2 ? count : 0;
    case VtgTopology::LineStrip:
        return count >= 2 ? count - 1 : 0;
    case VtgTopology::Triangles:
        return count / 3;
    case VtgTopology::TriangleStrip:
    case VtgTopology::TriangleFan:
        return count >= 3 ? count - 2 : 0;
    case VtgTopology::Quads:
        return (count / 4) * 2;
    case VtgTopology::QuadStrip:
        return count >= 4 ? ((count - 2) / 2) * 2 : 0;
    case VtgTopology::LinesAdjacency:
        return count / 4;
    case VtgTopology::LineStripAdjacency:
        return count >= 4 ? count - 3 : 0;
    case VtgTopology::TrianglesAdjacency:
        return count / 6;
    case VtgTopology::TriangleStripAdjacency:
        return count >= 6 ? (count - 4) / 2 : 0;
    }
    return 0;
}

/// Vertex produced by the vertex stage used as a vertex of a primitive
[[nodiscard]] constexpr u32 VtgPrimitiveVertex(VtgTopology topology, u32 primitive, u32 vertex,
                                               u32 count) noexcept {
    switch (topology) {
    case VtgTopology::Points:
        return primitive;
    case VtgTopology::Lines:
        return primitive * 2 + vertex;
    case VtgTopology::LineLoop:
        return count != 0 ? (primitive + vertex) % count : 0;
    case VtgTopology::LineStrip:
    case VtgTopology::LineStripAdjacency:
        return primitive + vertex;
    case VtgTopology::Triangles:
        return primitive * 3 + vertex;
    case VtgTopology::TriangleStrip: {
        // Odd triangles swap their last two vertices to keep the winding
        const u32 odd{primitive & 1};
        const u32 offset{vertex == 0 ? 0U : (vertex == 1 ? 1U + odd : 2U - odd)};
        return primitive + offset;
    }
    case VtgTopology::TriangleFan:
        return vertex == 0 ? 0 : primitive + vertex;
    case VtgTopology::Quads: {
        // Each quad is the triangles (0, 1, 2) and (0, 2, 3)
        const u32 second{primitive & 1};
        const u32 base{(primitive / 2) * 4};
        return vertex == 0 ? base : base + vertex + second;
    }
    case VtgTopology::QuadStrip: {
        // Each quad is the triangles (0, 1, 3) and (0, 3, 2)
        const u32 second{primitive & 1};
        const u32 base{(primitive / 2) * 2};
        const u32 offset{vertex == 0 ? 0U : (vertex == 1 ? 1U + second * 2U : 3U - second)};
        return base + offset;
    }
    case VtgTopology::LinesAdjacency:
        return primitive * 4 + vertex;
    case VtgTopology::TrianglesAdjacency:
        return primitive * 6 + vertex;
    case VtgTopology::TriangleStripAdjacency:
        return primitive * 2 + vertex;
    }
    return 0;
}

/// Layout in words of a vertex in the buffers shared between stages
struct VtgVaryingLayout {
    static constexpr u32 POSITION_WORDS = 4;
    static constexpr u32 GENERIC_WORDS = 4;
    static constexpr u32 CLIP_DISTANCE_WORDS = 8;
    static constexpr u32 NUM_GENERICS = 32;

    std::array<s32, NUM_GENERICS> generics{};
    s32 point_size{-1};
    s32 clip_distances{-1};
    s32 layer{-1};
    s32 viewport_index{-1};
    u32 stride{};

    explicit VtgVaryingLayout() {
        generics.fill(-1);
    }

    explicit VtgVaryingLayout(const VaryingState& stores) : VtgVaryingLayout() {
        stride = POSITION_WORDS;
        for (size_t index = 0; index < NUM_GENERICS; ++index) {
            if (stores.Generic(index)) {
                generics[index] = static_cast<s32>(stride);
                stride += GENERIC_WORDS;
            }
        }
        const auto reserve{[this](bool used, u32 words) {
            const s32 offset{used ? static_cast<s32>(stride) : -1};
            stride += used ? words : 0;
            return offset;
        }};
        point_size = reserve(stores[IR::Attribute::PointSize], 1);
        clip_distances = reserve(stores.ClipDistances(), CLIP_DISTANCE_WORDS);
        layer = reserve(stores[IR::Attribute::Layer], 1);
        viewport_index = reserve(stores[IR::Attribute::ViewportIndex], 1);
    }

    /// Word offset of an attribute inside a vertex, if the layout has it
    [[nodiscard]] std::optional<u32> Offset(IR::Attribute attribute) const {
        const auto at{[](s32 base, u32 element) -> std::optional<u32> {
            return base < 0 ? std::nullopt : std::optional<u32>{static_cast<u32>(base) + element};
        }};
        if (attribute >= IR::Attribute::PositionX && attribute <= IR::Attribute::PositionW) {
            return static_cast<u32>(attribute) - static_cast<u32>(IR::Attribute::PositionX);
        }
        if (IR::IsGeneric(attribute)) {
            return at(generics[IR::GenericAttributeIndex(attribute)],
                      IR::GenericAttributeElement(attribute));
        }
        if (attribute >= IR::Attribute::ClipDistance0 && attribute <= IR::Attribute::ClipDistance7) {
            return at(clip_distances, static_cast<u32>(attribute) -
                                          static_cast<u32>(IR::Attribute::ClipDistance0));
        }
        switch (attribute) {
        case IR::Attribute::PointSize:
            return at(point_size, 0);
        case IR::Attribute::Layer:
            return at(layer, 0);
        case IR::Attribute::ViewportIndex:
            return at(viewport_index, 0);
        default:
            return std::nullopt;
        }
    }
};

/// Storage buffers added to a vertex stage running as compute
struct VtgVertexBindings {
    /// Index of the first added storage buffer descriptor
    u32 base{};
    u32 draw_info{};
    u32 output{};
    u32 index_buffer{};
    /// Vertex buffers read by the stage and the storage buffer each one is bound to
    boost::container::static_vector<std::pair<u8, u32>, VtgDrawInfo::NUM_ATTRIBUTES> vertex_buffers;
};

/// Storage buffers added to a geometry stage running as compute
struct VtgGeometryBindings {
    /// Index of the first added storage buffer descriptor
    u32 base{};
    u32 draw_info{};
    u32 input{};
    u32 output{};
    u32 index_buffer{};
    /// Vertices and indices reserved for each input primitive
    u32 vertices_per_primitive{};
    u32 indices_per_primitive{};
    OutputTopology output_topology{};
};

} // namespace Shader
