// SPDX-License-Identifier: GPL-2.0-or-later

// Area sampling by Sam Belliveau and Filippo Tarpini (public domain).
// Adapted from the supplied Area v2 bundle; bilinear hardware filtering combines
// adjacent source texels without sacrificing their area-coverage weights.
// https://entropymine.com/imageworsener/pixelmixing/

#version 460 core

layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D color_texture;
#ifdef VULKAN
layout(push_constant) uniform AreaConstants { layout(offset = 80) vec4 area; uint paired_grid; };
#else
layout(location = 0) uniform vec4 area;
layout(location = 1) uniform uint paired_grid;
#endif
// area.xy: positive source footprint; area.zw: reciprocal texture size.

// Return the weighted sampling position (in source texels) and combined area
// for two consecutive source pixels. The sampler blends them in one fetch.
vec2 PairSample(float begin, float end, int first) {
    float pixel = float(first);
    float first_area = max(0.0, min(end, pixel + 1.0) - max(begin, pixel));
    float second_area = max(0.0, min(end, pixel + 2.0) - max(begin, pixel + 1.0));
    float total_area = first_area + second_area;
    return vec2(pixel + 0.5 + second_area / max(total_area, 1e-8), total_area);
}

void main() {
    vec2 footprint = area.xy;
    // Uniform fast path: aligned 2x uses a single fetch, 4x uses four.
    // No per-pixel coverage calculation or varying loop bounds on this path.
    if (paired_grid != 0u) {
        uvec2 pairs = uvec2(paired_grid & 65535u, paired_grid >> 16u);
        if (all(equal(pairs, uvec2(1u)))) {
            color = textureLod(color_texture, frag_tex_coord, 0.0);
            return;
        }
        vec2 step_uv = footprint * area.zw / vec2(pairs);
        vec2 first_uv = frag_tex_coord - 0.5 * (vec2(pairs) - 1.0) * step_uv;
        vec4 sum = vec4(0.0);
        for (uint y = 0u; y < pairs.y; ++y)
            for (uint x = 0u; x < pairs.x; ++x)
                sum += textureLod(color_texture, first_uv + vec2(x, y) * step_uv, 0.0);
        color = sum / float(pairs.x * pairs.y);
        return;
    }
    vec2 source_size = vec2(textureSize(color_texture, 0));
    vec2 begin = frag_tex_coord * source_size - 0.5 * footprint;
    vec2 end = begin + footprint;
    ivec2 first = ivec2(floor(begin));
    ivec2 count = ivec2(ceil(end)) - first;

    // A single filtered fetch suffices whenever at most two texels overlap
    // on each axis, regardless of scaling ratio or pixel-grid alignment.
    if (all(lessThanEqual(count, ivec2(2)))) {
        vec2 x = PairSample(begin.x, end.x, first.x);
        vec2 y = PairSample(begin.y, end.y, first.y);
        color = textureLod(color_texture, vec2(x.x, y.x) * area.zw, 0.0);
        return;
    }

    vec4 sum = vec4(0.0);
    for (int y = 0; y < count.y; y += 2) {
        vec2 py = PairSample(begin.y, end.y, first.y + y);
        for (int x = 0; x < count.x; x += 2) {
            vec2 px = PairSample(begin.x, end.x, first.x + x);
            sum += (px.y * py.y) *
                   textureLod(color_texture, vec2(px.x, py.x) * area.zw, 0.0);
        }
    }
    color = sum / (footprint.x * footprint.y);
}
