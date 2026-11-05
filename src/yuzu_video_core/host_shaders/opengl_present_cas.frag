// SPDX-License-Identifier: MIT
// Copyright (c) 2019 Advanced Micro Devices, Inc.
// Presentation adaptation of the FidelityFX CAS cross kernel. Uses the green
// channel coefficient, gamma-2 approximation for SDR, and a strength blend so
// the UI strength tends continuously to zero. No second sharpening with FSR.
layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D color_texture;
// Reconstruction flags: bit 0 = native, bit 1 = five taps, bit 2 = precomputed halo.
#ifdef VULKAN
layout(push_constant) uniform CasConstants {
    layout(offset = 80) float cas_strength;
    uint native_bypass;
};
#else
layout(location = 0) uniform float cas_strength;
layout(location = 1) uniform uint native_bypass;
#endif
#ifdef CAS_CATMULL
#include "catmull_rom.glsl"
#endif

vec4 CasSource(vec2 uv, vec2 size, vec2 inv_size) {
#ifdef CAS_CATMULL
    if ((native_bypass & 4u) != 0u) {
        // The halo is exactly one texel per output pixel. Avoid another interpolation.
        return texelFetch(color_texture, clamp(ivec2(uv * size), ivec2(0), ivec2(size) - 1), 0);
    }
    if ((native_bypass & 5u) == 0u) {
        return (native_bypass & 2u) != 0u
            ? SampleCatmullRomFast(color_texture, uv, size, inv_size)
            : SampleCatmullRom(color_texture, uv, size, inv_size);
    }
#endif
    return textureLod(color_texture, uv, 0.0);
}

void main() {
    vec2 size = vec2(textureSize(color_texture, 0));
    vec2 inv_size = 1.0 / size;
    // Neighbors must be one output pixel away, including cropped/flipped UVs.
    vec2 dx = dFdx(frag_tex_coord);
    vec2 dy = dFdy(frag_tex_coord);
    vec4 c = CasSource(frag_tex_coord, size, inv_size);
#ifdef CAS_QUAD
    // Adjacent fragments already reconstructed one horizontal and one vertical
    // neighbor. Quad operations also provide helper fragments along primitives.
    // Only the two neighbors outside this 2x2 quad need another texture lookup.
    vec3 horizontal = subgroupQuadSwapHorizontal(c.rgb);
    vec3 vertical = subgroupQuadSwapVertical(c.rgb);
    bool right = (gl_SubgroupInvocationID & 1u) != 0u;
    bool bottom = (gl_SubgroupInvocationID & 2u) != 0u;
    vec3 outside_x = CasSource(frag_tex_coord + (right ? dx : -dx), size, inv_size).rgb;
    vec3 outside_y = CasSource(frag_tex_coord + (bottom ? dy : -dy), size, inv_size).rgb;
    vec3 w = right ? horizontal : outside_x;
    vec3 e = right ? outside_x : horizontal;
    vec3 n = bottom ? outside_y : vertical;
    vec3 s = bottom ? vertical : outside_y;
#else
    vec3 n = CasSource(frag_tex_coord + dy, size, inv_size).rgb;
    vec3 s = CasSource(frag_tex_coord - dy, size, inv_size).rgb;
    vec3 w = CasSource(frag_tex_coord - dx, size, inv_size).rgb;
    vec3 e = CasSource(frag_tex_coord + dx, size, inv_size).rgb;
#endif
    vec3 center = c.rgb * c.rgb;
    n *= n; s *= s; w *= w; e *= e;
    vec3 mn = min(min(min(n, s), min(w, e)), center);
    vec3 mx = max(max(max(n, s), max(w, e)), center);
    vec3 amp = sqrt(clamp(min(mn, vec3(1.0) - mx) / max(mx, vec3(0.000001)), 0.0, 1.0));
    float strength = clamp(cas_strength, 0.0, 1.0);
    float weight = -amp.g / mix(8.0, 5.0, strength);
    vec3 sharp = clamp((center + weight * (n + s + w + e)) / (1.0 + 4.0 * weight), 0.0, 1.0);
    color = vec4(sqrt(mix(center, sharp, strength)), c.a);
}
