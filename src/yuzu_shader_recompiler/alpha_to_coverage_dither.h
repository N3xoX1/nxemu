// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include "yuzu_common/common_types.h"

namespace Shader {

// Interleaved Gradient Noise, Jorge Jimenez, SIGGRAPH 2014:
// https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/
// Evaluate at host pixel centers, never guest-resolution coordinates or a frame-dependent seed.
// Supersampling then averages fine coverage instead of enlarging guest-sized dither blocks.
constexpr f32 IGN_X = 0.06711056f;
constexpr f32 IGN_Y = 0.00583715f;
constexpr f32 IGN_MULTIPLIER = 52.9829189f;

inline f32 InterleavedGradientNoise(u32 x, u32 y) {
    const f32 gradient = (static_cast<f32>(x) + 0.5f) * IGN_X +
                         (static_cast<f32>(y) + 0.5f) * IGN_Y;
    const f32 value = IGN_MULTIPLIER * (gradient - std::floor(gradient));
    return value - std::floor(value);
}

inline bool AlphaToCoverageDitherKeeps(f32 alpha, u32 x, u32 y) {
    // Ordered comparison also drops NaN/negative/zero alpha, and keeps alpha >= 1.
    return alpha > InterleavedGradientNoise(x, y);
}

} // namespace Shader
