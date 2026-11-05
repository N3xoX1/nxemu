// SPDX-FileCopyrightText: Copyright 2021 Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

#extension GL_EXT_shader_explicit_arithmetic_types : require

layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D color_texture;

layout(push_constant) uniform EasuConstants {
    layout(offset = 16) vec4 first_vertex;
    layout(offset = 80) uvec4 con0;
    uvec4 con1;
    uvec4 con2;
};

#define A_GPU 1
#define A_GLSL 1
#ifdef YUZU_USE_FP16
#define A_HALF 1
#endif
#include "ffx_a.h"

#ifdef YUZU_USE_FP16
#define FSR_EASU_H 1
AH4 FsrEasuRH(AF2 p) { return AH4(textureGather(color_texture, p, 0)); }
AH4 FsrEasuGH(AF2 p) { return AH4(textureGather(color_texture, p, 1)); }
AH4 FsrEasuBH(AF2 p) { return AH4(textureGather(color_texture, p, 2)); }
#else
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(color_texture, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(color_texture, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(color_texture, p, 2); }
#endif
#include "ffx_fsr1.h"

void main() {
    if (con0.x == 0u) {
        color = texture(color_texture, frag_tex_coord);
        return;
    }

    const uvec4 con3 = uvec4(0u, floatBitsToUint(4.0 * uintBitsToFloat(con1.y)), 0u, 0u);
    const uvec2 pos = uvec2(max(gl_FragCoord.xy - first_vertex.xy, vec2(0.0)));
#ifdef YUZU_USE_FP16
    AH3 rgb;
    FsrEasuH(rgb, pos, con0, con1, con2, con3);
    color = vec4(rgb, texture(color_texture, frag_tex_coord).a);
#else
    AF3 rgb;
    FsrEasuF(rgb, pos, con0, con1, con2, con3);
    color = vec4(rgb, texture(color_texture, frag_tex_coord).a);
#endif
}
