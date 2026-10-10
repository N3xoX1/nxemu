// SPDX-License-Identifier: GPL-2.0-or-later

#version 450
layout(binding = 0) uniform sampler2D source_tex;
layout(location = 0) out vec4 color;
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
    color = texelFetch(source_tex, source_coord(), 0);
}
