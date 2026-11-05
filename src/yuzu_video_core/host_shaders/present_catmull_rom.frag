// SPDX-License-Identifier: GPL-2.0-or-later
#version 460 core
#extension GL_GOOGLE_include_directive : enable

layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D color_texture;
// Reconstruction flags: bit 0 = native, bit 1 = five taps, bit 2 = precomputed halo.
#ifdef VULKAN
layout(push_constant) uniform CatmullConstants { layout(offset = 84) uint native_bypass; };
#else
layout(location = 1) uniform uint native_bypass;
#endif
#include "catmull_rom.glsl"

void main() {
    // Uniform per layer. A full 1:1 mapping, including flips, is already exact.
    if ((native_bypass & 1u) != 0u) {
        color = textureLod(color_texture, frag_tex_coord, 0.0);
        return;
    }
    vec2 size = vec2(textureSize(color_texture, 0));
    color = (native_bypass & 2u) != 0u
        ? SampleCatmullRomFast(color_texture, frag_tex_coord, size, 1.0 / size)
        : SampleCatmullRom(color_texture, frag_tex_coord, size, 1.0 / size);
}
