// SPDX-License-Identifier: GPL-2.0-or-later

// Area sampling by Sam Belliveau and Filippo Tarpini (public domain).
// Based on Eden's Area filter, using bilinear hardware filtering to combine
// adjacent source texels without sacrificing their area-coverage weights.
// https://entropymine.com/imageworsener/pixelmixing/

#version 460 core

layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D color_texture;

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
    vec2 source_size = vec2(textureSize(color_texture, 0));

    // Derive the source-space footprint from the interpolated UVs. Unlike
    // Eden's screen-size uniform / push-constant approach this also handles
    // cropping, flipped UVs and any letterboxing without extra uniforms.
    vec2 footprint = source_size * fwidth(frag_tex_coord);
    if (any(lessThanEqual(footprint, vec2(1e-6)))) {
        color = textureLod(color_texture, frag_tex_coord, 0.0);
        return;
    }

    vec2 begin = frag_tex_coord * source_size - 0.5 * footprint;
    vec2 end = begin + footprint;
    ivec2 first = ivec2(floor(begin));
    ivec2 count = ivec2(ceil(end)) - first;

    // A single filtered fetch suffices whenever at most two texels overlap
    // on each axis, regardless of scaling ratio or pixel-grid alignment.
    if (all(lessThanEqual(count, ivec2(2)))) {
        vec2 x = PairSample(begin.x, end.x, first.x);
        vec2 y = PairSample(begin.y, end.y, first.y);
        color = textureLod(color_texture, vec2(x.x, y.x) / source_size, 0.0);
        return;
    }

    vec4 sum = vec4(0.0);
    for (int y = 0; y < count.y; y += 2) {
        vec2 py = PairSample(begin.y, end.y, first.y + y);
        for (int x = 0; x < count.x; x += 2) {
            vec2 px = PairSample(begin.x, end.x, first.x + x);
            sum += (px.y * py.y) *
                   textureLod(color_texture, vec2(px.x, py.x) / source_size, 0.0);
        }
    }
    color = sum / (footprint.x * footprint.y);
}
