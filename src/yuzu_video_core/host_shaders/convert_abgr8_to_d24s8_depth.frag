// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout(binding = 0) uniform sampler2D color_texture;

// Depth half of convert_abgr8_to_d24s8.frag, for devices without shader stencil export.
void main() {
    uvec4 bytes = uvec4(round(texelFetch(color_texture, ivec2(gl_FragCoord.xy), 0) * 255.0));
    uint depth = bytes.r | (bytes.g << 8) | (bytes.b << 16);
    gl_FragDepth = float(depth) / 16777215.0;
}
