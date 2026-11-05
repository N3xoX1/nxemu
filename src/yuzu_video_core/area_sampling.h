// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VideoCore
{
// Even integer reductions aligned to source pixel boundaries have equally
// weighted 2-texel pairs. A 1:1 axis also admits one exact linear fetch.
// Validate total span, not just the ratio: ratio error accumulates over pixels.
inline std::uint32_t AreaSamplingGrid(unsigned width, unsigned height, float left, float top,
                                      float right, float bottom, unsigned output_width,
                                      unsigned output_height)
{
    const auto axis = [](unsigned size, float start, float end, unsigned output) -> unsigned {
        if (!size || !output)
            return 0;
        const double begin = std::min(double(start), double(end)) * size;
        const double span = std::abs(double(end) - start) * size;
        const double ratio = std::round(span / output);
        if (!std::isfinite(begin) || !std::isfinite(span) || ratio < 1 || ratio > 65534 ||
            std::abs(span - ratio * output) > 0.001 ||
            (ratio != 1 &&
             (std::fmod(ratio, 2.) != 0 || std::abs(begin - std::round(begin)) > 0.001)))
            return 0;
        return ratio == 1 ? 1 : static_cast<unsigned>(ratio) / 2;
    };
    const unsigned x = axis(width, left, right, output_width);
    const unsigned y = axis(height, top, bottom, output_height);
    return x && y ? x | (y << 16) : 0;
}
} // namespace VideoCore
