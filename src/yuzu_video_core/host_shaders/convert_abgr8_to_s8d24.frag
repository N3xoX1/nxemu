// SPDX-License-Identifier: GPL-2.0-or-later

#version 450
#extension GL_ARB_shader_stencil_export : require

layout(binding = 0) uniform sampler2D color_texture;

void main() {
    uvec4 bytes = uvec4(round(texelFetch(color_texture, ivec2(gl_FragCoord.xy), 0) * 255.0));
    // Inverse of convert_s8d24_to_abgr8.frag: RGBA = depth[23:16], depth[15:8], depth[7:0], stencil.
    uint depth = bytes.b | (bytes.g << 8) | (bytes.r << 16);
    gl_FragDepth = float(depth) / 16777215.0;
    gl_FragStencilRefARB = int(bytes.a);
}
