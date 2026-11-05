// SPDX-FileCopyrightText: Copyright 2021 Advanced Micro Devices, Inc.
// SPDX-License-Identifier: MIT

#extension GL_EXT_shader_explicit_arithmetic_types : require

layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D color_texture;

layout(push_constant) uniform RcasConstants {
    layout(offset = 80) uvec4 rcas_constants;
};

#define A_GPU 1
#define A_GLSL 1
#define FSR_RCAS_PASSTHROUGH_ALPHA 1

#ifdef YUZU_USE_FP16
#define A_HALF 1
#endif
#include "ffx_a.h"

#ifdef YUZU_USE_FP16
#define FSR_RCAS_H 1
AH4 FsrRcasLoadH(ASW2 p) { return AH4(texelFetch(color_texture, clamp(ivec2(p), ivec2(0), textureSize(color_texture, 0) - ivec2(1)), 0)); }
void FsrRcasInputH(inout AH1 r, inout AH1 g, inout AH1 b) {}
#else
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return texelFetch(color_texture, clamp(ivec2(p), ivec2(0), textureSize(color_texture, 0) - ivec2(1)), 0); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#endif
#include "ffx_fsr1.h"

void main() {
    const uvec2 p = uvec2(frag_tex_coord * vec2(textureSize(color_texture, 0)));
#ifdef YUZU_USE_FP16
    AH4 c;
    FsrRcasH(c.r, c.g, c.b, c.a, p, rcas_constants);
    color = vec4(c);
#else
    AF4 c;
    FsrRcasF(c.r, c.g, c.b, c.a, p, rcas_constants);
    color = c;
#endif
}
