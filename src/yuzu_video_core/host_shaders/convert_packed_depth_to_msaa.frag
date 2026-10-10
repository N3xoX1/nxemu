// SPDX-License-Identifier: GPL-2.0-or-later

#version 450
layout(binding = 0) uniform usampler2D source_tex;
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
    uvec2 packed = texelFetch(source_tex, source_coord(), 0).rg;
    uint stencil;
    if (packed_format == 0) { // D24_UNORM_S8_UINT (Maxwell S8Z24): depth in the low 24 bits.
        gl_FragDepth = float(packed.r & 0x00ffffffu) / 16777215.0;
        stencil = packed.r >> 24;
    } else if (packed_format == 1) { // S8_UINT_D24_UNORM (Maxwell Z24S8): stencil in the low byte.
        gl_FragDepth = float(packed.r >> 8) / 16777215.0;
        stencil = packed.r & 255u;
    } else { // D32_FLOAT_S8_UINT
        gl_FragDepth = uintBitsToFloat(packed.r);
        stencil = packed.g & 255u;
    }
}
