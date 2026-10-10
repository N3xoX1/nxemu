// SPDX-License-Identifier: GPL-2.0-or-later

#version 450
#extension GL_ARB_shader_stencil_export : require

layout(binding = 0) uniform sampler2DMS source_tex;
layout(binding = 1) uniform usampler2DMS stencil_tex;
layout(constant_id = 0) const int destination_samples = 1;
layout(constant_id = 2) const int source_samples = 4;

layout(location = 0) in vec2 texcoord;

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

ivec2 source_coord() {
    return clamp(ivec2(floor(texcoord)), ivec2(0), textureSize(source_tex) - 1);
}

void main() {
    // Depth and stencil cannot be averaged. With one destination sample this reads sample 0.
    ivec2 coord = source_coord();
    int sample_id = source_sample(source_samples);
    gl_FragDepth = texelFetch(source_tex, coord, sample_id).r;
    gl_FragStencilRefARB = int(texelFetch(stencil_tex, coord, sample_id).r);
}
