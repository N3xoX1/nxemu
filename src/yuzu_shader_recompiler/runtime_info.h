// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <optional>
#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_shader_recompiler/frontend/ir/attribute.h"
#include "yuzu_shader_recompiler/varying_state.h"

namespace Shader {

enum class AttributeType : u8 {
    Float,
    SignedInt,
    UnsignedInt,
    SignedScaled,
    UnsignedScaled,
    Disabled,
};

enum class InputTopology {
    Points,
    Lines,
    LinesAdjacency,
    Triangles,
    TrianglesAdjacency,
};

struct InputTopologyVertices {
    static u32 vertices(InputTopology input_topology) {
        switch (input_topology) {
        case InputTopology::Lines:
            return 2;
        case InputTopology::LinesAdjacency:
            return 4;
        case InputTopology::Triangles:
            return 3;
        case InputTopology::TrianglesAdjacency:
            return 6;
        case InputTopology::Points:
        default:
            return 1;
        }
    }
};

enum class CompareFunction {
    Never,
    Less,
    Equal,
    LessThanEqual,
    Greater,
    NotEqual,
    GreaterThanEqual,
    Always,
};

enum class TessPrimitive {
    Isolines,
    Triangles,
    Quads,
};

enum class TessSpacing {
    Equal,
    FractionalOdd,
    FractionalEven,
};

struct TransformFeedbackVarying {
    u32 buffer{};
    u32 stride{};
    u32 offset{};
    u32 components{};
};

struct RuntimeInfo {
    std::array<AttributeType, 32> generic_input_types{};
    VaryingState previous_stage_stores;
    std::map<IR::Attribute, IR::Attribute> previous_stage_legacy_stores_mapping;

    bool convert_depth_mode{};
    bool force_early_z{};

    TessPrimitive tess_primitive{};
    TessSpacing tess_spacing{};
    bool tess_clockwise{};

    /// Generic output copied into the layer at the end of the vertex stage, standing in for an
    /// eliminated geometry shader
    std::optional<IR::Attribute> forwarded_layer_attribute;

    InputTopology input_topology{};

    std::optional<float> fixed_state_point_size;
    std::optional<CompareFunction> alpha_test_func;
    float alpha_test_reference{};

    /// Static Y negate value
    bool y_negate{};
    /// Use storage buffers instead of global pointers on GLASM
    bool glasm_use_storage_buffers{};

    /// Transform feedback state for each varying
    std::array<TransformFeedbackVarying, 256> xfb_varyings{};
    u32 xfb_count{0};

    // Vulkan requires fragment outputs to match the attachment scalar type.
    // Float remains the default for other backends.
    std::array<AttributeType, 8> color_output_types{};

    /// The first render target blends with a second source color taken from color output 1
    bool dual_source_blend{};
};

} // namespace Shader
