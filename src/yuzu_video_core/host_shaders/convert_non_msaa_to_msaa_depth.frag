// SPDX-License-Identifier: GPL-2.0-or-later

#version 450
layout(binding = 0) uniform sampler2D source_tex;
layout(push_constant) uniform PushConstants {
    ivec2 dst_offset;
    ivec2 src_offset;
    ivec2 scale;
    int packed_format;
};

ivec2 source_coord() {
    // Maxwell stores samples in a spatial grid, with X varying first.
    return src_offset + (ivec2(gl_FragCoord.xy) - dst_offset) * scale +
           ivec2(gl_SampleID % scale.x, gl_SampleID / scale.x);
}

void main() {
    ivec2 coord = source_coord();
    gl_FragDepth = texelFetch(source_tex, coord, 0).r;
}
