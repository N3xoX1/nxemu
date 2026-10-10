// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout(binding = 0) uniform sampler2DMS source_tex;
layout(constant_id = 0) const int destination_samples = 1;
layout(constant_id = 2) const int source_samples = 4;
layout(constant_id = 1) const int linear_filter = 0;

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 color;

// Maxwell stores samples in a spatial grid, with X varying first.
ivec2 grid(int samples) {
    int x = samples >= 8 ? 4 : (samples >= 2 ? 2 : 1);
    return ivec2(x, samples / x);
}

// Source sample that sits at the position of the destination sample being shaded.
int source_sample(int samples) {
    ivec2 src_grid = grid(samples);
    ivec2 dst_grid = grid(destination_samples);
    ivec2 dst_sample = ivec2(gl_SampleID % dst_grid.x, gl_SampleID / dst_grid.x);
    ivec2 src_sample = min(src_grid - 1, dst_sample * src_grid / dst_grid);
    return src_sample.x + src_sample.y * src_grid.x;
}

vec4 fetch_color(ivec2 coord, int samples) {
    coord = clamp(coord, ivec2(0), textureSize(source_tex) - 1);
    if (destination_samples == 1) {
        vec4 result = vec4(0.0);
        for (int i = 0; i < samples; ++i) {
            result += texelFetch(source_tex, coord, i);
        }
        return result / float(samples);
    }
    return texelFetch(source_tex, coord, source_sample(samples));
}

void main() {
    int samples = source_samples;
    if (linear_filter != 0) {
        // texcoord is in texels; the centre of texel (0,0) is (0.5,0.5).
        vec2 position = texcoord - vec2(0.5);
        ivec2 base = ivec2(floor(position));
        vec2 weight = fract(position);
        vec4 top = mix(fetch_color(base, samples),
                       fetch_color(base + ivec2(1, 0), samples), weight.x);
        vec4 bottom = mix(fetch_color(base + ivec2(0, 1), samples),
                          fetch_color(base + ivec2(1, 1), samples), weight.x);
        color = mix(top, bottom, weight.y);
    } else {
        color = fetch_color(ivec2(floor(texcoord)), samples);
    }
}
