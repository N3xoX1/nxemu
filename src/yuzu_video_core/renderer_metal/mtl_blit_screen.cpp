// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <set>
#include <cmath>
#include <cstdlib>
#include <string>

#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_blit_screen.h"
#include "yuzu_video_core/smaa_area_tex.h"
#include "yuzu_video_core/smaa_search_tex.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_video_core/renderer_metal/mtl_shader_util.h"

namespace Metal {
namespace {
// Buffer slot the crop rectangle is pushed into with setVertexBytes.
constexpr u32 CROP_BUFFER_INDEX = 1;
// Fragment-side constant, whether the source needs decoding before the
// drawable's fixed-function sRGB encode. See Draw().
constexpr u32 SOURCE_IS_LINEAR_INDEX = 1;
// Fragment-side constant, cubic B and C in xy, source texels per output pixel in
// zw. Bound for every filter, read only by the ones that need it.
constexpr u32 FILTER_PARAMS_INDEX = 2;
constexpr u32 SOURCE_TEXTURE_INDEX = 0;
constexpr u32 SOURCE_SAMPLER_INDEX = 0;

// Present shaders. Every pass draws one oversized triangle, no diagonal seam, no
// vertex buffer, positions come from the vertex id.
constexpr const char* PRESENT_MSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

struct CropRect {
    float left;
    float top;
    float right;
    float bottom;
};

vertex VertexOut present_vertex(uint vid [[vertex_id]],
                                constant CropRect& crop [[buffer(1)]]) {
    // Clip-space triangle covering [-1,1] with UVs covering [0,1].
    const float2 positions[3] = { float2(-1.0, -3.0), float2(-1.0, 1.0), float2(3.0, 1.0) };
    const float2 uvs[3]       = { float2( 0.0,  2.0), float2( 0.0, 0.0), float2(2.0, 0.0) };

    VertexOut out;
    out.position = float4(positions[vid], 0.0, 1.0);
    // Map the unit UV through the crop rectangle so a framebuffer wider than
    // the visible area is cropped rather than squashed.
    out.uv = float2(mix(crop.left, crop.right, uvs[vid].x),
                    mix(crop.top,  crop.bottom, uvs[vid].y));
    return out;
}

// The drawable is sRGB, so the texture unit encodes linear -> sRGB on write.
// A source that is not an sRGB format hands back bits that are already
// sRGB-encoded, so they have to be decoded here or the encode that follows
// washes the picture out.
inline float3 decode_srgb(float3 c) {
    return select(c / 12.92f, pow((c + 0.055f) / 1.055f, float3(2.4f)), c > 0.04045f);
}

/// One tap, decoded, so filters weight linear values.
inline float4 fetch_texel(texture2d<float> source, sampler s, float2 uv, uint source_is_linear) {
    float4 c = source.sample(s, uv);
    if (source_is_linear != 0u) {
        c.rgb = decode_srgb(c.rgb);
    }
    return c;
}

/// Spline-1: one bilinear fetch, positioned so the hardware's own linear
/// interpolation performs a smoothstep between texel centres. Cheaper than a
/// cubic and close enough to one to be worth having. Needs a *linear* sampler -
/// with nearest it degenerates to point sampling.
fragment float4 present_spline1(VertexOut in [[stage_in]],
                                texture2d<float> source [[texture(0)]],
                                sampler source_sampler [[sampler(0)]],
                                constant uint& source_is_linear [[buffer(1)]]) {
    const float2 size = float2(source.get_width(), source.get_height());
    const float2 t = fract(in.uv * size + 0.5f);
    const float2 uv = (floor(in.uv * size + 0.5f) + smoothstep(0.0f, 1.0f, t) - 0.5f) / size;
    return fetch_texel(source, source_sampler, uv, source_is_linear);
}

// MMPX works on exact texel equality, so every tap is a point sample of the
// source grid regardless of the sampler the pass was given.
inline float4 mmpx_tap(texture2d<float> source, sampler s, float2 coord, float2 inv_size,
                       float dx, float dy) {
    return source.sample(s, coord + float2(dx, dy) * inv_size, level(0.0f));
}

inline bool mmpx_same(float4 a, float4 b) {
    return all(a == b);
}

inline bool mmpx_diff(float4 a, float4 b) {
    return any(a != b);
}

inline float mmpx_luma(float4 c) {
    return dot(c.rgb, float3(0.2126f, 0.7152f, 0.0722f)) * (1.0f - c.a);
}

/// MMPX: a pixel-art doubler that reconstructs edges from exact colour matches
/// rather than by interpolating. Ported from present_mmpx.frag; the tap grid,
/// the rules and their order are kept identical to it.
fragment float4 present_mmpx(VertexOut in [[stage_in]],
                             texture2d<float> source [[texture(0)]],
                             sampler source_sampler [[sampler(0)]],
                             constant uint& source_is_linear [[buffer(1)]]) {
    const float2 size = float2(source.get_width(), source.get_height());
    const float2 inv_size = 1.0f / size;
    const float2 pos = fract(in.uv * size) - 0.5f;
    const float2 coord = in.uv - pos * inv_size;

    #define SRC(x, y) mmpx_tap(source, source_sampler, coord, inv_size, (x), (y))

    const float4 E = SRC(0.0f, 0.0f);
    const float4 A = SRC(-1.0f, -1.0f);
    const float4 B = SRC(0.0f, -1.0f);
    const float4 C = SRC(1.0f, -1.0f);
    const float4 D = SRC(-1.0f, 0.0f);
    const float4 F = SRC(1.0f, 0.0f);
    const float4 G = SRC(-1.0f, 1.0f);
    const float4 H = SRC(0.0f, 1.0f);
    const float4 I = SRC(1.0f, 1.0f);

    float4 J = E, K = E, L = E, M = E;

    if (mmpx_same(E, A) && mmpx_same(E, B) && mmpx_same(E, C) && mmpx_same(E, D) &&
        mmpx_same(E, F) && mmpx_same(E, G) && mmpx_same(E, H) && mmpx_same(E, I)) {
        float4 flat_colour = E;
        if (source_is_linear != 0u) {
            flat_colour.rgb = decode_srgb(flat_colour.rgb);
        }
        return flat_colour;
    }

    const float4 P = SRC(0.0f, 2.0f);
    const float4 Q = SRC(-2.0f, 0.0f);
    const float4 R = SRC(2.0f, 0.0f);
    const float4 S = SRC(0.0f, 2.0f);

    const float Bl = mmpx_luma(B);
    const float Dl = mmpx_luma(D);
    const float El = mmpx_luma(E);
    const float Fl = mmpx_luma(F);
    const float Hl = mmpx_luma(H);

    const bool any_eq3_ACG = mmpx_same(E, A) || mmpx_same(E, C) || mmpx_same(E, G);
    const bool any_eq3_ACI = mmpx_same(E, A) || mmpx_same(E, C) || mmpx_same(E, I);
    const bool any_eq3_AGI = mmpx_same(E, A) || mmpx_same(E, G) || mmpx_same(E, I);
    const bool any_eq3_CGI = mmpx_same(E, C) || mmpx_same(E, G) || mmpx_same(E, I);

    if (mmpx_same(D, B) && mmpx_diff(D, H) && mmpx_diff(D, F) &&
        (El >= Dl || mmpx_same(E, A)) && any_eq3_ACG &&
        (El < Dl || mmpx_diff(A, D) || mmpx_diff(E, P) || mmpx_diff(E, Q))) {
        J = mix(D, J, 0.5f);
    }
    if (mmpx_same(B, F) && mmpx_diff(B, D) && mmpx_diff(B, H) &&
        (El >= Bl || mmpx_same(E, C)) && any_eq3_ACI &&
        (El < Bl || mmpx_diff(C, B) || mmpx_diff(E, P) || mmpx_diff(E, R))) {
        K = mix(B, K, 0.5f);
    }
    if (mmpx_same(H, D) && mmpx_diff(H, F) && mmpx_diff(H, B) &&
        (El >= Hl || mmpx_same(E, G)) && any_eq3_AGI &&
        (El < Hl || mmpx_diff(G, H) || mmpx_diff(E, S) || mmpx_diff(E, Q))) {
        L = mix(H, L, 0.5f);
    }
    if (mmpx_same(F, H) && mmpx_diff(F, B) && mmpx_diff(F, D) &&
        (El >= Fl || mmpx_same(E, I)) && any_eq3_CGI &&
        (El < Fl || mmpx_diff(I, H) || mmpx_diff(E, R) || mmpx_diff(E, S))) {
        M = mix(F, M, 0.5f);
    }

    if (mmpx_diff(E, F) && mmpx_same(E, C) && mmpx_same(E, I) && mmpx_same(E, D) &&
        mmpx_same(E, Q) && mmpx_same(F, B) && mmpx_same(F, H) &&
        mmpx_diff(F, SRC(3.0f, 0.0f))) {
        M = mix(M, F, 0.5f);
        K = mix(K, M, 0.5f);
    }
    if (mmpx_diff(E, D) && mmpx_same(E, A) && mmpx_same(E, G) && mmpx_same(E, F) &&
        mmpx_same(E, R) && mmpx_same(D, B) && mmpx_same(D, H) &&
        mmpx_diff(D, SRC(-3.0f, 0.0f))) {
        L = mix(L, D, 0.5f);
        J = mix(J, L, 0.5f);
    }
    if (mmpx_diff(E, H) && mmpx_same(E, G) && mmpx_same(E, I) && mmpx_same(E, B) &&
        mmpx_same(E, P) && mmpx_same(H, D) && mmpx_same(H, F) &&
        mmpx_diff(H, SRC(0.0f, 3.0f))) {
        M = mix(M, H, 0.5f);
        L = mix(L, M, 0.5f);
    }
    if (mmpx_diff(E, B) && mmpx_same(E, A) && mmpx_same(E, C) && mmpx_same(E, H) &&
        mmpx_same(E, S) && mmpx_same(B, D) && mmpx_same(B, F) &&
        mmpx_diff(B, SRC(0.0f, -3.0f))) {
        K = mix(K, B, 0.5f);
        J = mix(J, K, 0.5f);
    }

    if (Bl < El && mmpx_same(E, G) && mmpx_same(E, H) && mmpx_same(E, I) && mmpx_same(E, S) &&
        mmpx_diff(E, A) && mmpx_diff(E, D) && mmpx_diff(E, C) && mmpx_diff(E, F)) {
        K = mix(K, B, 0.5f);
        J = mix(J, K, 0.5f);
    }
    if (Hl < El && mmpx_same(E, A) && mmpx_same(E, B) && mmpx_same(E, C) && mmpx_same(E, P) &&
        mmpx_diff(E, D) && mmpx_diff(E, G) && mmpx_diff(E, I) && mmpx_diff(E, F)) {
        M = mix(M, H, 0.5f);
        L = mix(L, M, 0.5f);
    }
    if (Fl < El && mmpx_same(E, A) && mmpx_same(E, D) && mmpx_same(E, G) && mmpx_same(E, Q) &&
        mmpx_diff(E, B) && mmpx_diff(E, C) && mmpx_diff(E, I) && mmpx_diff(E, H)) {
        M = mix(M, F, 0.5f);
        K = mix(K, M, 0.5f);
    }
    if (Dl < El && mmpx_same(E, C) && mmpx_same(E, F) && mmpx_same(E, I) && mmpx_same(E, R) &&
        mmpx_diff(E, B) && mmpx_diff(E, A) && mmpx_diff(E, G) && mmpx_diff(E, H)) {
        L = mix(L, D, 0.5f);
        J = mix(J, L, 0.5f);
    }

    if (mmpx_diff(H, B)) {
        if (mmpx_diff(H, A) && mmpx_diff(H, E) && mmpx_diff(H, C)) {
            if (mmpx_same(H, G) && mmpx_same(H, F) && mmpx_same(H, R) && mmpx_diff(H, D) &&
                mmpx_diff(H, SRC(2.0f, -1.0f))) {
                L = mix(M, L, 0.5f);
            }
            if (mmpx_same(H, I) && mmpx_same(H, D) && mmpx_same(H, Q) && mmpx_diff(H, F) &&
                mmpx_diff(H, SRC(-2.0f, -1.0f))) {
                M = mix(L, M, 0.5f);
            }
        }
        if (mmpx_diff(B, I) && mmpx_diff(B, G) && mmpx_diff(B, E)) {
            if (mmpx_same(B, A) && mmpx_same(B, F) && mmpx_same(B, R) && mmpx_diff(B, D) &&
                mmpx_diff(B, SRC(2.0f, 1.0f))) {
                J = mix(K, L, 0.5f);
            }
            if (mmpx_same(B, C) && mmpx_same(B, D) && mmpx_same(B, Q) && mmpx_diff(B, F) &&
                mmpx_diff(B, SRC(-2.0f, 1.0f))) {
                K = mix(J, K, 0.5f);
            }
        }
    }

    if (mmpx_diff(F, D)) {
        if (mmpx_diff(D, I) && mmpx_diff(D, E) && mmpx_diff(D, C)) {
            if (mmpx_same(D, A) && mmpx_same(D, H) && mmpx_same(D, S) && mmpx_diff(D, B) &&
                mmpx_diff(D, SRC(1.0f, 2.0f))) {
                J = mix(L, J, 0.5f);
            }
            if (mmpx_same(D, G) && mmpx_same(D, B) && mmpx_same(D, P) && mmpx_diff(D, H) &&
                mmpx_diff(D, SRC(1.0f, 2.0f))) {
                L = mix(J, L, 0.5f);
            }
        }
        if (mmpx_diff(F, E) && mmpx_diff(F, A) && mmpx_diff(F, G)) {
            if (mmpx_same(F, C) && mmpx_same(F, H) && mmpx_same(F, S) && mmpx_diff(F, B) &&
                mmpx_diff(F, SRC(-1.0f, 2.0f))) {
                K = mix(M, K, 0.5f);
            }
            if (mmpx_same(F, I) && mmpx_same(F, B) && mmpx_same(F, P) && mmpx_diff(F, H) &&
                mmpx_diff(F, SRC(-1.0f, -2.0f))) {
                M = mix(K, M, 0.5f);
            }
        }
    }

    #undef SRC

    const float2 a = fract(in.uv * size);
    float4 colour = (a.x < 0.5f) ? (a.y < 0.5f ? J : L) : (a.y < 0.5f ? K : M);
    if (source_is_linear != 0u) {
        colour.rgb = decode_srgb(colour.rgb);
    }
    return colour;
}

inline float4 sgsr_weight_y(float4 dx, float4 dy, float4 std) {
    const float4 x = ((dx * dx) + (dy * dy)) * 0.55f + std;
    return (x - 1.0f) * (x - 4.0f) * 3.8125f; // approx. of (x - 1) * (x - 4)^3
}

/// Snapdragon Game Super Resolution 1.0, mobile variant, operating on the
/// green channel as a luma proxy (OPERATION_MODE 1). Ported from
/// sgsr1_shader_mobile.frag. Metal's gather returns components in the same
/// order GLSL's textureGather does, so the tap indices carry over unchanged.
fragment float4 present_sgsr(VertexOut in [[stage_in]],
                             texture2d<float> source [[texture(0)]],
                             sampler source_sampler [[sampler(0)]],
                             constant uint& source_is_linear [[buffer(1)]],
                             constant float4& params [[buffer(2)]]) {
    constexpr float EDGE_THRESHOLD = 8.0f / 255.0f;
    const float edge_sharpness = params.x;

    const float2 size = float2(source.get_width(), source.get_height());
    const float2 scale = 1.0f / size;

    float4 colour = source.sample(source_sampler, in.uv, level(0.0f));

    const float2 icoord = in.uv * size + float2(-0.5f, 0.5f);
    const float2 icoord_pixel = floor(icoord);
    const float2 coord = icoord_pixel * scale;
    const float2 pl = icoord - icoord_pixel;

    float4 dg[3];
    dg[0] = source.gather(source_sampler, coord, int2(0), component::y);
    dg[1] = source.gather(source_sampler, coord + float2(2.0f * scale.x, 0.0f), int2(0),
                          component::y);
    dg[2] = float4(source.gather(source_sampler, coord + float2(scale.x, -scale.y), int2(0),
                                 component::y).wz,
                   source.gather(source_sampler, coord + float2(scale.x, scale.y), int2(0),
                                 component::y).yx);

    const float edge_vote =
        abs(dg[0].z - dg[0].y) + abs(colour.y - dg[0].y) + abs(colour.y - dg[0].z);
    if (edge_vote > EDGE_THRESHOLD) {
        const float mean = (dg[0].y + dg[0].z + dg[1].x + dg[1].w) * 0.25f;
        dg[0] -= mean;
        dg[1] -= mean;
        dg[2] -= mean;

        const float4 sum = abs(dg[0]) + abs(dg[1]) + abs(dg[2]);
        const float std = 2.181818f / (sum.x + sum.y + sum.z + sum.w);

        const float4 w0 =
            sgsr_weight_y(pl.xxxx + float4(1.0f, 0.0f, 0.0f, 1.0f),
                          pl.yyyy + float4(-1.0f, -1.0f, 0.0f, 0.0f),
                          clamp(abs(dg[0]) * std, 0.0f, 1.0f)) +
            sgsr_weight_y(pl.xxxx + float4(-1.0f, -2.0f, -2.0f, -1.0f),
                          pl.yyyy + float4(-1.0f, -1.0f, 0.0f, 0.0f),
                          clamp(abs(dg[1]) * std, 0.0f, 1.0f)) +
            sgsr_weight_y(pl.xxxx + float4(0.0f, -1.0f, -1.0f, 0.0f),
                          pl.yyyy + float4(1.0f, 1.0f, -2.0f, -2.0f),
                          clamp(abs(dg[2]) * std, 0.0f, 1.0f));
        const float4 w1 = dg[0] + dg[1] + dg[2];

        const float min_y = min(min(dg[0].y, dg[0].z), min(dg[1].x, dg[1].w));
        const float max_y = max(max(dg[0].y, dg[0].z), max(dg[1].x, dg[1].w));
        const float fv_x = w0.x + w0.y + w0.z + w0.w;
        const float fv_y = w1.x + w1.y + w1.z + w1.w;

        const float fy = clamp((fv_y / fv_x) * edge_sharpness, min_y, max_y);
        const float dy = clamp(fy - colour.y + mean, -23.0f / 255.0f, 23.0f / 255.0f);
        colour = clamp(colour + dy, 0.0f, 1.0f);
    }

    colour.w = 1.0f;
    if (source_is_linear != 0u) {
        colour.rgb = decode_srgb(colour.rgb);
    }
    return colour;
}

inline float sgsr_fast_lanczos2(float x) {
    float wA = x - 4.0f;
    const float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}

inline float2 sgsr_edge_weight_y(float dx, float dy, float c, float3 data) {
    const float std = data.x;
    const float2 dir = data.yz;
    const float edge_dis = (dx * dir.y) + (dy * dir.x);
    const float x = ((dx * dx) + (dy * dy)) +
                    ((edge_dis * edge_dis) * ((clamp((c * c) * std, 0.0f, 1.0f) * 0.7f) - 1.0f));
    const float w = sgsr_fast_lanczos2(x);
    return float2(w, w * c);
}

inline float2 sgsr_edge_direction(float4 left, float4 right) {
    const float rx_lz = right.x - left.z;
    const float rw_ly = right.w - left.y;
    const float2 delta = float2(rx_lz + rw_ly, rx_lz - rw_ly);
    const float length_inv =
        rsqrt((delta.x * delta.x + 3.075740e-05f) + (delta.y * delta.y));
    return delta * length_inv;
}

/// SGSR 1.0, edge-direction variant. A different reconstruction from the plain
/// mobile path - it estimates an edge direction and weights taps along it -
/// which is why it is a separate shader rather than a flag.
fragment float4 present_sgsr_edge(VertexOut in [[stage_in]],
                                  texture2d<float> source [[texture(0)]],
                                  sampler source_sampler [[sampler(0)]],
                                  constant uint& source_is_linear [[buffer(1)]],
                                  constant float4& params [[buffer(2)]]) {
    constexpr float EDGE_THRESHOLD = 8.0f / 255.0f;
    const float edge_sharpness = params.x;

    const float2 size = float2(source.get_width(), source.get_height());
    const float2 scale = 1.0f / size;

    float4 colour = source.sample(source_sampler, in.uv, level(0.0f));

    const float2 img_coord = in.uv * size + float2(-0.5f, 0.5f);
    const float2 img_coord_pixel = floor(img_coord);
    float2 coord = img_coord_pixel * scale;
    const float2 pl = img_coord - img_coord_pixel;

    float4 left = source.gather(source_sampler, coord, int2(0), component::y);
    const float edge_vote =
        abs(left.z - left.y) + abs(colour.y - left.y) + abs(colour.y - left.z);
    if (edge_vote > EDGE_THRESHOLD) {
        coord.x += scale.x;

        float4 right =
            source.gather(source_sampler, coord + float2(scale.x, 0.0f), int2(0), component::y);
        float4 up_down;
        up_down.xy = source.gather(source_sampler, coord + float2(0.0f, -scale.y), int2(0),
                                   component::y).wz;
        up_down.zw = source.gather(source_sampler, coord + float2(0.0f, scale.y), int2(0),
                                   component::y).yx;

        const float mean = (left.y + left.z + right.x + right.w) * 0.25f;
        left -= mean;
        right -= mean;
        up_down -= mean;
        const float centre = colour.y - mean;

        const float sum = abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w) +
                          abs(right.x) + abs(right.y) + abs(right.z) + abs(right.w) +
                          abs(up_down.x) + abs(up_down.y) + abs(up_down.z) + abs(up_down.w);
        const float sum_mean = 1.014185e+01f / sum;
        const float std = sum_mean * sum_mean;

        const float3 data = float3(std, sgsr_edge_direction(left, right));
        float2 a_wy = sgsr_edge_weight_y(pl.x, pl.y + 1.0f, up_down.x, data);
        a_wy += sgsr_edge_weight_y(pl.x - 1.0f, pl.y + 1.0f, up_down.y, data);
        a_wy += sgsr_edge_weight_y(pl.x - 1.0f, pl.y - 2.0f, up_down.z, data);
        a_wy += sgsr_edge_weight_y(pl.x, pl.y - 2.0f, up_down.w, data);
        a_wy += sgsr_edge_weight_y(pl.x + 1.0f, pl.y - 1.0f, left.x, data);
        a_wy += sgsr_edge_weight_y(pl.x, pl.y - 1.0f, left.y, data);
        a_wy += sgsr_edge_weight_y(pl.x, pl.y, left.z, data);
        a_wy += sgsr_edge_weight_y(pl.x + 1.0f, pl.y, left.w, data);
        a_wy += sgsr_edge_weight_y(pl.x - 1.0f, pl.y - 1.0f, right.x, data);
        a_wy += sgsr_edge_weight_y(pl.x - 2.0f, pl.y - 1.0f, right.y, data);
        a_wy += sgsr_edge_weight_y(pl.x - 2.0f, pl.y, right.z, data);
        a_wy += sgsr_edge_weight_y(pl.x - 1.0f, pl.y, right.w, data);

        const float final_y = a_wy.y / a_wy.x;
        const float max_y = max(max(left.y, left.z), max(right.x, right.w));
        const float min_y = min(min(left.y, left.z), min(right.x, right.w));
        float delta_y = clamp(edge_sharpness * final_y, min_y, max_y) - centre;
        delta_y = clamp(delta_y, -23.0f / 255.0f, 23.0f / 255.0f);

        colour.xyz = clamp(colour.xyz + delta_y, 0.0f, 1.0f);
    }

    colour.w = 1.0f;
    if (source_is_linear != 0u) {
        colour.rgb = decode_srgb(colour.rgb);
    }
    return colour;
}

// ---------------------------------------------------------------------------
// AMD FidelityFX Super Resolution 1.0, ported from externals/FidelityFX-FSR.
//
// Two passes: EASU reconstructs the image at output resolution, RCAS sharpens
// it. The non-packed fp32 path, since Apple GPUs run fp32 at full rate.
// FsrEasuCon's constants are recomputed in the shader from the texture sizes
// instead of being passed in, which keeps the shared uniform layout.
// ---------------------------------------------------------------------------

inline float fsr_rcp(float x) {
    return 1.0f / x;
}

inline half3 fsr_min3h(half3 a, half3 b, half3 c) {
    return min(a, min(b, c));
}

inline half3 fsr_max3h(half3 a, half3 b, half3 c) {
    return max(a, max(b, c));
}

/// One tap of the EASU kernel: a Lanczos-2 approximation shaped by the local
/// gradient direction and anisotropy (FP16 for 2x ALU throughput on Apple GPUs).
inline void fsr_easu_tap(thread half3& colour, thread half& weight, half2 off, half2 dir,
                         half2 len, half lob, half clp, half3 tap) {
    half2 v;
    v.x = (off.x * dir.x) + (off.y * dir.y);
    v.y = (off.x * -dir.y) + (off.y * dir.x);
    v *= len;
    half d2 = min(v.x * v.x + v.y * v.y, clp);
    half wB = half(2.0h / 5.0h) * d2 - 1.0h;
    half wA = lob * d2 - 1.0h;
    wB *= wB;
    wA *= wA;
    wB = half(25.0h / 16.0h) * wB - half(25.0h / 16.0h - 1.0h);
    const half w = wB * wA;
    colour += tap * w;
    weight += w;
}

/// Accumulates the '+' gradient direction and its length for one of the four
/// bilinear corners.
inline void fsr_easu_set(thread float2& dir, thread float& len, float w,
                         half lA, half lB, half lC, half lD, half lE) {
    const float dc = float(lD - lC);
    const float cb = float(lC - lB);
    float lenX = max(abs(dc), abs(cb));
    lenX = fsr_rcp(max(lenX, 1.0e-6f));
    const float dirX = float(lD - lB);
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;

    const float ec = float(lE - lC);
    const float ca = float(lC - lA);
    float lenY = max(abs(ec), abs(ca));
    lenY = fsr_rcp(max(lenY, 1.0e-6f));
    const float dirY = float(lE - lA);
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

/// EASU: edge-adaptive spatial upsampling. Writes linear colour at output
/// resolution; RCAS below consumes it (or writes directly to the drawable for FsrEasu).
fragment float4 present_fsr_easu(VertexOut in [[stage_in]],
                                 texture2d<float> source [[texture(0)]],
                                 sampler source_sampler [[sampler(0)]],
                                 constant uint& source_is_linear [[buffer(1)]],
                                 constant float4& params [[buffer(2)]]) {
    const float2 src_size = float2(source.get_width(), source.get_height());
    const float2 rcp_src = 1.0f / src_size;

    // Position of 'f' in source pixels. in.uv already carries the crop, so the
    // viewport mapping FsrEasuCon folds into con0 is applied here instead.
    const float2 pp_full = in.uv * src_size - 0.5f;
    const float2 fp = floor(pp_full);
    const float2 pp = pp_full - fp;

    // Gather centres, exactly the offsets FsrEasuCon builds.
    const float2 p0 = (fp + float2(1.0f, -1.0f)) * rcp_src;
    const float2 p1 = p0 + float2(-1.0f, 2.0f) * rcp_src;
    const float2 p2 = p0 + float2(1.0f, 2.0f) * rcp_src;
    const float2 p3 = p0 + float2(0.0f, 4.0f) * rcp_src;

    const half4 bczzR = half4(source.gather(source_sampler, p0, int2(0), component::x));
    const half4 bczzG = half4(source.gather(source_sampler, p0, int2(0), component::y));
    const half4 bczzB = half4(source.gather(source_sampler, p0, int2(0), component::z));
    const half4 ijfeR = half4(source.gather(source_sampler, p1, int2(0), component::x));
    const half4 ijfeG = half4(source.gather(source_sampler, p1, int2(0), component::y));
    const half4 ijfeB = half4(source.gather(source_sampler, p1, int2(0), component::z));
    const half4 klhgR = half4(source.gather(source_sampler, p2, int2(0), component::x));
    const half4 klhgG = half4(source.gather(source_sampler, p2, int2(0), component::y));
    const half4 klhgB = half4(source.gather(source_sampler, p2, int2(0), component::z));
    const half4 zzonR = half4(source.gather(source_sampler, p3, int2(0), component::x));
    const half4 zzonG = half4(source.gather(source_sampler, p3, int2(0), component::y));
    const half4 zzonB = half4(source.gather(source_sampler, p3, int2(0), component::z));

    // Approximate luma, twice the usual scale, as the reference does.
    const half4 bczzL = bczzB * 0.5h + (bczzR * 0.5h + bczzG);
    const half4 ijfeL = ijfeB * 0.5h + (ijfeR * 0.5h + ijfeG);
    const half4 klhgL = klhgB * 0.5h + (klhgR * 0.5h + klhgG);
    const half4 zzonL = zzonB * 0.5h + (zzonR * 0.5h + zzonG);

    const half bL = bczzL.x, cL = bczzL.y;
    const half iL = ijfeL.x, jL = ijfeL.y, fL = ijfeL.z, eL = ijfeL.w;
    const half kL = klhgL.x, lL = klhgL.y, hL = klhgL.z, gL = klhgL.w;
    const half oL = zzonL.z, nL = zzonL.w;

    float2 dir = float2(0.0f);
    float len = 0.0f;
    fsr_easu_set(dir, len, (1.0f - pp.x) * (1.0f - pp.y), bL, eL, fL, gL, jL);
    fsr_easu_set(dir, len, pp.x * (1.0f - pp.y), cL, fL, gL, hL, kL);
    fsr_easu_set(dir, len, (1.0f - pp.x) * pp.y, fL, iL, jL, kL, nL);
    fsr_easu_set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    const float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    const bool zro = dirR < (1.0f / 32768.0f);
    dirR = rsqrt(max(dirR, 1.0e-12f));
    dirR = zro ? 1.0f : dirR;
    dir.x = zro ? 1.0f : dir.x;
    dir *= dirR;

    len = len * 0.5f;
    len *= len;

    const float stretch = (dir.x * dir.x + dir.y * dir.y) *
                          fsr_rcp(max(max(abs(dir.x), abs(dir.y)), 1.0e-6f));
    const half2 dir_h = half2(dir);
    const half2 pp_h = half2(pp);
    const half2 len2 = half2(1.0f + (stretch - 1.0f) * len, 1.0f - 0.5f * len);
    const half lob = half(0.5f + ((1.0f / 4.0f - 0.04f) - 0.5f) * len);
    const half clp = half(fsr_rcp(float(lob)));

    // Dering against the four nearest taps.
    const half3 f_tap = half3(ijfeR.z, ijfeG.z, ijfeB.z);
    const half3 g_tap = half3(klhgR.w, klhgG.w, klhgB.w);
    const half3 j_tap = half3(ijfeR.y, ijfeG.y, ijfeB.y);
    const half3 k_tap = half3(klhgR.x, klhgG.x, klhgB.x);
    const half3 min4 = min(fsr_min3h(f_tap, g_tap, j_tap), k_tap);
    const half3 max4 = max(fsr_max3h(f_tap, g_tap, j_tap), k_tap);

    half3 aC = half3(0.0h);
    half aW = 0.0h;
    fsr_easu_tap(aC, aW, half2(0.0h, -1.0h) - pp_h, dir_h, len2, lob, clp,
                 half3(bczzR.x, bczzG.x, bczzB.x));
    fsr_easu_tap(aC, aW, half2(1.0h, -1.0h) - pp_h, dir_h, len2, lob, clp,
                 half3(bczzR.y, bczzG.y, bczzB.y));
    fsr_easu_tap(aC, aW, half2(-1.0h, 1.0h) - pp_h, dir_h, len2, lob, clp,
                 half3(ijfeR.x, ijfeG.x, ijfeB.x));
    fsr_easu_tap(aC, aW, half2(0.0h, 1.0h) - pp_h, dir_h, len2, lob, clp, j_tap);
    fsr_easu_tap(aC, aW, half2(0.0h, 0.0h) - pp_h, dir_h, len2, lob, clp, f_tap);
    fsr_easu_tap(aC, aW, half2(-1.0h, 0.0h) - pp_h, dir_h, len2, lob, clp,
                 half3(ijfeR.w, ijfeG.w, ijfeB.w));
    fsr_easu_tap(aC, aW, half2(1.0h, 1.0h) - pp_h, dir_h, len2, lob, clp, k_tap);
    fsr_easu_tap(aC, aW, half2(2.0h, 1.0h) - pp_h, dir_h, len2, lob, clp,
                 half3(klhgR.y, klhgG.y, klhgB.y));
    fsr_easu_tap(aC, aW, half2(2.0h, 0.0h) - pp_h, dir_h, len2, lob, clp,
                 half3(klhgR.z, klhgG.z, klhgB.z));
    fsr_easu_tap(aC, aW, half2(1.0h, 0.0h) - pp_h, dir_h, len2, lob, clp, g_tap);
    fsr_easu_tap(aC, aW, half2(1.0h, 2.0h) - pp_h, dir_h, len2, lob, clp,
                 half3(zzonR.z, zzonG.z, zzonB.z));
    fsr_easu_tap(aC, aW, half2(0.0h, 2.0h) - pp_h, dir_h, len2, lob, clp,
                 half3(zzonR.w, zzonG.w, zzonB.w));

    float3 pix = float3(min(max4, max(min4, aC / aW)));
    if (source_is_linear != 0u) {
        pix = decode_srgb(pix);
    }
    return float4(pix, 1.0f);
}

static float3 rcas_load(texture2d<float> source, int2 sp, int2 size, int2 offset) {
    const int2 c = clamp(sp + offset, int2(0), size - 1);
    return source.read(uint2(c)).rgb;
}

/// RCAS: robust contrast-adaptive sharpening, run on EASU's output at display
/// resolution. params.x carries sharpness in stops, already converted to linear
/// by the caller.
fragment float4 present_fsr_rcas(VertexOut in [[stage_in]],
                                 texture2d<float> source [[texture(0)]],
                                 sampler source_sampler [[sampler(0)]],
                                 constant uint& source_is_linear [[buffer(1)]],
                                 constant float4& params [[buffer(2)]]) {
    constexpr float FSR_RCAS_LIMIT = 0.25f - (1.0f / 16.0f);
    const float sharp = params.x;

    const int2 size = int2(source.get_width(), source.get_height());
    const int2 sp = int2(in.uv * float2(size));

    const float3 b = rcas_load(source, sp, size, int2(0, -1));
    const float3 d = rcas_load(source, sp, size, int2(-1, 0));
    const float3 e = rcas_load(source, sp, size, int2(0, 0));
    const float3 f = rcas_load(source, sp, size, int2(1, 0));
    const float3 h = rcas_load(source, sp, size, int2(0, 1));

    const float bL = b.b * 0.5f + (b.r * 0.5f + b.g);
    const float dL = d.b * 0.5f + (d.r * 0.5f + d.g);
    const float eL = e.b * 0.5f + (e.r * 0.5f + e.g);
    const float fL = f.b * 0.5f + (f.r * 0.5f + f.g);
    const float hL = h.b * 0.5f + (h.r * 0.5f + h.g);

    // Noise detection, used to back the sharpening off on flat noisy areas.
    float nz = 0.25f * bL + 0.25f * dL + 0.25f * fL + 0.25f * hL - eL;
    const float range = max(max(max(bL, dL), max(eL, fL)), hL) -
                        min(min(min(bL, dL), min(eL, fL)), hL);
    nz = saturate(abs(nz) * fsr_rcp(max(range, 1.0e-6f)));
    nz = -0.5f * nz + 1.0f;

    const float3 mn4 = min(min(b, d), min(f, h));
    const float3 mx4 = max(max(b, d), max(f, h));
    const float2 peak = float2(1.0f, -4.0f);

    const float3 hit_min = min(mn4, e) / max(4.0f * mx4, float3(1.0e-6f));
    const float3 hit_max = (peak.x - max(mx4, e)) / min(4.0f * mn4 + peak.y, float3(-1.0e-6f));
    const float3 lobe_rgb = max(-hit_min, hit_max);
    float lobe = max(-FSR_RCAS_LIMIT, min(max(max(lobe_rgb.r, lobe_rgb.g), lobe_rgb.b), 0.0f)) *
                 sharp;
    lobe *= nz;

    const float rcp_l = fsr_rcp(4.0f * lobe + 1.0f);
    float3 pix = (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcp_l;

    if (source_is_linear != 0u) {
        pix = decode_srgb(pix);
    }
    return float4(pix, 1.0f);
}

// ---------------------------------------------------------------------------
// SMAA 1x, ULTRA preset. Ported from host_shaders/opengl_smaa.glsl, which is
// Jorge Jimenez et al.'s reference implementation.
//
// Three passes, each with its own entry point below: colour edge detection into
// an RG target, blending-weight calculation into an RGBA target (this is the
// pass that needs the two precomputed lookup textures), then neighbourhood
// blending back into the frame.
//
// The offsets the reference computes in its vertex shaders are computed in the
// fragment shaders here instead, because the present pipeline has one shared
// vertex function for every filter and cannot carry SMAA-only varyings.
// ---------------------------------------------------------------------------

constant float SMAA_THRESHOLD = 0.05f;
constant int SMAA_MAX_SEARCH_STEPS = 32;
constant int SMAA_MAX_SEARCH_STEPS_DIAG = 16;
constant float SMAA_CORNER_ROUNDING_NORM = 25.0f / 100.0f;
constant float SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR = 2.0f;
constant float SMAA_AREATEX_MAX_DISTANCE = 16.0f;
constant float SMAA_AREATEX_MAX_DISTANCE_DIAG = 20.0f;
constant float2 SMAA_AREATEX_PIXEL_SIZE = float2(1.0f / 160.0f, 1.0f / 560.0f);
constant float SMAA_AREATEX_SUBTEX_SIZE = 1.0f / 7.0f;
constant float2 SMAA_SEARCHTEX_SIZE = float2(66.0f, 33.0f);
constant float2 SMAA_SEARCHTEX_PACKED_SIZE = float2(64.0f, 16.0f);

inline void smaa_movc2(bool2 cond, thread float2& variable, float2 value) {
    if (cond.x) variable.x = value.x;
    if (cond.y) variable.y = value.y;
}

inline void smaa_movc4(bool4 cond, thread float4& variable, float4 value) {
    if (cond.x) variable.x = value.x;
    if (cond.y) variable.y = value.y;
    if (cond.z) variable.z = value.z;
    if (cond.w) variable.w = value.w;
}

/// Pass 1: colour edge detection. Writes RG - a horizontal and a vertical edge
/// flag - and returns zero where the reference discards, since this pass owns
/// its target and clears it to zero anyway.
fragment float2 present_smaa_edges(VertexOut in [[stage_in]],
                                   texture2d<float> source [[texture(0)]],
                                   sampler source_sampler [[sampler(0)]]) {
    const float2 rt = 1.0f / float2(source.get_width(), source.get_height());
    const float2 uv = in.uv;
    const float4 offset0 = fma(rt.xyxy, float4(-1.0f, 0.0f, 0.0f, -1.0f), uv.xyxy);
    const float4 offset1 = fma(rt.xyxy, float4(1.0f, 0.0f, 0.0f, 1.0f), uv.xyxy);
    const float4 offset2 = fma(rt.xyxy, float4(-2.0f, 0.0f, 0.0f, -2.0f), uv.xyxy);

    const float2 threshold = float2(SMAA_THRESHOLD);
    float4 delta;
    const float3 C = source.sample(source_sampler, uv, level(0.0f)).rgb;

    float3 t = abs(C - source.sample(source_sampler, offset0.xy, level(0.0f)).rgb);
    delta.x = max(max(t.r, t.g), t.b);
    t = abs(C - source.sample(source_sampler, offset0.zw, level(0.0f)).rgb);
    delta.y = max(max(t.r, t.g), t.b);

    float2 edges = step(threshold, delta.xy);
    if (dot(edges, float2(1.0f)) == 0.0f) {
        return float2(0.0f);
    }

    t = abs(C - source.sample(source_sampler, offset1.xy, level(0.0f)).rgb);
    delta.z = max(max(t.r, t.g), t.b);
    t = abs(C - source.sample(source_sampler, offset1.zw, level(0.0f)).rgb);
    delta.w = max(max(t.r, t.g), t.b);

    float2 max_delta = max(delta.xy, delta.zw);

    t = abs(C - source.sample(source_sampler, offset2.xy, level(0.0f)).rgb);
    delta.z = max(max(t.r, t.g), t.b);
    t = abs(C - source.sample(source_sampler, offset2.zw, level(0.0f)).rgb);
    delta.w = max(max(t.r, t.g), t.b);

    max_delta = max(max_delta.xy, delta.zw);
    const float final_delta = max(max_delta.x, max_delta.y);

    edges.xy *= step(final_delta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);
    return edges;
}

inline float2 smaa_decode_diag2(float2 e) {
    e.r = e.r * abs(5.0f * e.r - 5.0f * 0.75f);
    return round(e);
}

inline float4 smaa_decode_diag4(float4 e) {
    e.rb = e.rb * abs(5.0f * e.rb - 5.0f * 0.75f);
    return round(e);
}

inline float2 smaa_search_diag1(texture2d<float> edges, sampler s, float2 rt, float2 uv,
                                float2 dir, thread float2& e) {
    float4 coord = float4(uv, -1.0f, 1.0f);
    const float3 t = float3(rt, 1.0f);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9f) {
        coord.xyz = fma(t, float3(dir, 1.0f), coord.xyz);
        e = edges.sample(s, coord.xy, level(0.0f)).rg;
        coord.w = dot(e, float2(0.5f));
    }
    return coord.zw;
}

inline float2 smaa_search_diag2(texture2d<float> edges, sampler s, float2 rt, float2 uv,
                                float2 dir, thread float2& e) {
    float4 coord = float4(uv, -1.0f, 1.0f);
    coord.x += 0.25f * rt.x;
    const float3 t = float3(rt, 1.0f);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9f) {
        coord.xyz = fma(t, float3(dir, 1.0f), coord.xyz);
        e = edges.sample(s, coord.xy, level(0.0f)).rg;
        e = smaa_decode_diag2(e);
        coord.w = dot(e, float2(0.5f));
    }
    return coord.zw;
}

inline float2 smaa_area_diag(texture2d<float> area_tex, sampler s, float2 dist, float2 e,
                             float offset) {
    float2 uv = fma(float2(SMAA_AREATEX_MAX_DISTANCE_DIAG), e, dist);
    uv = fma(SMAA_AREATEX_PIXEL_SIZE, uv, 0.5f * SMAA_AREATEX_PIXEL_SIZE);
    uv.x += 0.5f;
    uv.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return area_tex.sample(s, uv, level(0.0f)).rg;
}

inline float2 smaa_calculate_diag_weights(texture2d<float> edges, texture2d<float> area_tex,
                                          sampler s, float2 rt, float2 uv, float2 e,
                                          float4 subsample) {
    float2 weights = float2(0.0f);
    float4 d;
    float2 end;

    if (e.r > 0.0f) {
        d.xz = smaa_search_diag1(edges, s, rt, uv, float2(-1.0f, 1.0f), end);
        d.x += float(end.y > 0.9f);
    } else {
        d.xz = float2(0.0f);
    }
    d.yw = smaa_search_diag1(edges, s, rt, uv, float2(1.0f, -1.0f), end);

    if (d.x + d.y > 2.0f) {
        const float4 coords = fma(float4(-d.x + 0.25f, d.x, d.y, -d.y - 0.25f), rt.xyxy, uv.xyxy);
        float4 c;
        c.xy = edges.sample(s, coords.xy, level(0.0f), int2(-1, 0)).rg;
        c.zw = edges.sample(s, coords.zw, level(0.0f), int2(1, 0)).rg;
        const float4 decoded = smaa_decode_diag4(c);
        c = float4(decoded.y, decoded.x, decoded.w, decoded.z);
        float2 cc = fma(float2(2.0f), c.xz, c.yw);
        smaa_movc2(bool2(step(0.9f, d.zw)), cc, float2(0.0f));
        weights += smaa_area_diag(area_tex, s, d.xy, cc, subsample.z);
    }

    d.xz = smaa_search_diag2(edges, s, rt, uv, float2(-1.0f, -1.0f), end);
    if (edges.sample(s, uv, level(0.0f), int2(1, 0)).r > 0.0f) {
        d.yw = smaa_search_diag2(edges, s, rt, uv, float2(1.0f, 1.0f), end);
        d.y += float(end.y > 0.9f);
    } else {
        d.yw = float2(0.0f);
    }

    if (d.x + d.y > 2.0f) {
        const float4 coords = fma(float4(-d.x, -d.x, d.y, d.y), rt.xyxy, uv.xyxy);
        float4 c;
        c.x = edges.sample(s, coords.xy, level(0.0f), int2(-1, 0)).g;
        c.y = edges.sample(s, coords.xy, level(0.0f), int2(0, -1)).r;
        c.zw = edges.sample(s, coords.zw, level(0.0f), int2(1, 0)).gr;
        float2 cc = fma(float2(2.0f), c.xz, c.yw);
        smaa_movc2(bool2(step(0.9f, d.zw)), cc, float2(0.0f));
        weights += smaa_area_diag(area_tex, s, d.xy, cc, subsample.w).gr;
    }
    return weights;
}

inline float smaa_search_length(texture2d<float> search_tex, sampler s, float2 e, float offset) {
    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5f, -1.0f);
    float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0f);
    scale += float2(-1.0f, 1.0f);
    bias += float2(0.5f, -0.5f);
    scale *= 1.0f / SMAA_SEARCHTEX_PACKED_SIZE;
    bias *= 1.0f / SMAA_SEARCHTEX_PACKED_SIZE;
    return search_tex.sample(s, fma(scale, e, bias), level(0.0f)).r;
}

inline float smaa_search_x_left(texture2d<float> edges, texture2d<float> search_tex, sampler s,
                                float2 rt, float2 uv, float end) {
    float2 e = float2(0.0f, 1.0f);
    while (uv.x > end && e.g > 0.8281f && e.r == 0.0f) {
        e = edges.sample(s, uv, level(0.0f)).rg;
        uv = fma(-float2(2.0f, 0.0f), rt, uv);
    }
    const float offset = fma(-(255.0f / 127.0f), smaa_search_length(search_tex, s, e, 0.0f), 3.25f);
    return fma(rt.x, offset, uv.x);
}

inline float smaa_search_x_right(texture2d<float> edges, texture2d<float> search_tex, sampler s,
                                 float2 rt, float2 uv, float end) {
    float2 e = float2(0.0f, 1.0f);
    while (uv.x < end && e.g > 0.8281f && e.r == 0.0f) {
        e = edges.sample(s, uv, level(0.0f)).rg;
        uv = fma(float2(2.0f, 0.0f), rt, uv);
    }
    const float offset = fma(-(255.0f / 127.0f), smaa_search_length(search_tex, s, e, 0.5f), 3.25f);
    return fma(-rt.x, offset, uv.x);
}

inline float smaa_search_y_up(texture2d<float> edges, texture2d<float> search_tex, sampler s,
                              float2 rt, float2 uv, float end) {
    float2 e = float2(1.0f, 0.0f);
    while (uv.y > end && e.r > 0.8281f && e.g == 0.0f) {
        e = edges.sample(s, uv, level(0.0f)).rg;
        uv = fma(-float2(0.0f, 2.0f), rt, uv);
    }
    const float offset =
        fma(-(255.0f / 127.0f), smaa_search_length(search_tex, s, e.gr, 0.0f), 3.25f);
    return fma(rt.y, offset, uv.y);
}

inline float smaa_search_y_down(texture2d<float> edges, texture2d<float> search_tex, sampler s,
                                float2 rt, float2 uv, float end) {
    float2 e = float2(1.0f, 0.0f);
    while (uv.y < end && e.r > 0.8281f && e.g == 0.0f) {
        e = edges.sample(s, uv, level(0.0f)).rg;
        uv = fma(float2(0.0f, 2.0f), rt, uv);
    }
    const float offset =
        fma(-(255.0f / 127.0f), smaa_search_length(search_tex, s, e.gr, 0.5f), 3.25f);
    return fma(-rt.y, offset, uv.y);
}

inline float2 smaa_area(texture2d<float> area_tex, sampler s, float2 dist, float e1, float e2,
                        float offset) {
    float2 uv = fma(float2(SMAA_AREATEX_MAX_DISTANCE), round(4.0f * float2(e1, e2)), dist);
    uv = fma(SMAA_AREATEX_PIXEL_SIZE, uv, 0.5f * SMAA_AREATEX_PIXEL_SIZE);
    uv.y = fma(SMAA_AREATEX_SUBTEX_SIZE, offset, uv.y);
    return area_tex.sample(s, uv, level(0.0f)).rg;
}

inline void smaa_corner_horizontal(texture2d<float> edges, sampler s, thread float2& weights,
                                   float4 uv, float2 d) {
    const float2 left_right = step(d.xy, d.yx);
    float2 rounding = (1.0f - SMAA_CORNER_ROUNDING_NORM) * left_right;
    rounding /= left_right.x + left_right.y;
    float2 factor = float2(1.0f);
    factor.x -= rounding.x * edges.sample(s, uv.xy, level(0.0f), int2(0, 1)).r;
    factor.x -= rounding.y * edges.sample(s, uv.zw, level(0.0f), int2(1, 1)).r;
    factor.y -= rounding.x * edges.sample(s, uv.xy, level(0.0f), int2(0, -2)).r;
    factor.y -= rounding.y * edges.sample(s, uv.zw, level(0.0f), int2(1, -2)).r;
    weights *= saturate(factor);
}

inline void smaa_corner_vertical(texture2d<float> edges, sampler s, thread float2& weights,
                                 float4 uv, float2 d) {
    const float2 left_right = step(d.xy, d.yx);
    float2 rounding = (1.0f - SMAA_CORNER_ROUNDING_NORM) * left_right;
    rounding /= left_right.x + left_right.y;
    float2 factor = float2(1.0f);
    factor.x -= rounding.x * edges.sample(s, uv.xy, level(0.0f), int2(1, 0)).g;
    factor.x -= rounding.y * edges.sample(s, uv.zw, level(0.0f), int2(1, 1)).g;
    factor.y -= rounding.x * edges.sample(s, uv.xy, level(0.0f), int2(-2, 0)).g;
    factor.y -= rounding.y * edges.sample(s, uv.zw, level(0.0f), int2(-2, 1)).g;
    weights *= saturate(factor);
}

/// Pass 2: blending weights. edges at texture(0), the area LUT at texture(1),
/// the search LUT at texture(2).
fragment float4 present_smaa_weights(VertexOut in [[stage_in]],
                                     texture2d<float> edges [[texture(0)]],
                                     texture2d<float> area_tex [[texture(1)]],
                                     texture2d<float> search_tex [[texture(2)]],
                                     sampler s [[sampler(0)]]) {
    const float2 size = float2(edges.get_width(), edges.get_height());
    const float2 rt = 1.0f / size;
    const float2 uv = in.uv;
    const float2 pixcoord = uv * size;
    const float4 subsample = float4(0.0f); // SMAA 1x

    const float4 offset0 = fma(rt.xyxy, float4(-0.25f, -0.125f, 1.25f, -0.125f), uv.xyxy);
    const float4 offset1 = fma(rt.xyxy, float4(-0.125f, -0.25f, -0.125f, 1.25f), uv.xyxy);
    const float4 offset2 =
        fma(float4(rt.x, rt.x, rt.y, rt.y),
            float4(-2.0f, 2.0f, -2.0f, 2.0f) * float(SMAA_MAX_SEARCH_STEPS),
            float4(offset0.x, offset0.z, offset1.y, offset1.w));

    float4 weights = float4(0.0f);
    float2 e = edges.sample(s, uv).rg;

    if (e.g > 0.0f) {
        weights.rg = smaa_calculate_diag_weights(edges, area_tex, s, rt, uv, e, subsample);
        if (weights.r == -weights.g) {
            float2 d;
            float3 coords;
            coords.x = smaa_search_x_left(edges, search_tex, s, rt, offset0.xy, offset2.x);
            coords.y = offset1.y;
            d.x = coords.x;
            const float e1 = edges.sample(s, coords.xy, level(0.0f)).r;
            coords.z = smaa_search_x_right(edges, search_tex, s, rt, offset0.zw, offset2.y);
            d.y = coords.z;
            d = abs(round(fma(float2(size.x), d, -pixcoord.xx)));
            const float2 sqrt_d = sqrt(d);
            const float e2 = edges.sample(s, coords.zy, level(0.0f), int2(1, 0)).r;
            weights.rg = smaa_area(area_tex, s, sqrt_d, e1, e2, subsample.y);
            coords.y = uv.y;
            float2 w = weights.rg;
            smaa_corner_horizontal(edges, s, w, float4(coords.x, coords.y, coords.z, coords.y), d);
            weights.rg = w;
        } else {
            e.r = 0.0f;
        }
    }

    if (e.r > 0.0f) {
        float2 d;
        float3 coords;
        coords.y = smaa_search_y_up(edges, search_tex, s, rt, offset1.xy, offset2.z);
        coords.x = offset0.x;
        d.x = coords.y;
        const float e1 = edges.sample(s, coords.xy, level(0.0f)).g;
        coords.z = smaa_search_y_down(edges, search_tex, s, rt, offset1.zw, offset2.w);
        d.y = coords.z;
        d = abs(round(fma(float2(size.y), d, -pixcoord.yy)));
        const float2 sqrt_d = sqrt(d);
        const float e2 = edges.sample(s, coords.xz, level(0.0f), int2(0, 1)).g;
        weights.ba = smaa_area(area_tex, s, sqrt_d, e1, e2, subsample.x);
        coords.x = uv.x;
        float2 w = weights.ba;
        smaa_corner_vertical(edges, s, w, float4(coords.x, coords.y, coords.x, coords.z), d);
        weights.ba = w;
    }

    return weights;
}

/// Pass 3: neighbourhood blending. The frame at texture(0), the weights at
/// texture(1). This is the pass that writes the presented image, so it also
/// carries the sRGB decode every other present path does.
fragment float4 present_smaa_blend(VertexOut in [[stage_in]],
                                   texture2d<float> source [[texture(0)]],
                                   texture2d<float> blend_tex [[texture(1)]],
                                   sampler s [[sampler(0)]],
                                   constant uint& source_is_linear [[buffer(1)]]) {
    const float2 rt = 1.0f / float2(source.get_width(), source.get_height());
    const float2 uv = in.uv;
    const float4 offset = fma(rt.xyxy, float4(1.0f, 0.0f, 0.0f, 1.0f), uv.xyxy);

    float4 a;
    a.x = blend_tex.sample(s, offset.xy).a;
    a.y = blend_tex.sample(s, offset.zw).g;
    a.wz = blend_tex.sample(s, uv).xz;

    float4 colour;
    if (dot(a, float4(1.0f)) < 1e-5f) {
        colour = source.sample(s, uv, level(0.0f));
    } else {
        const bool h = max(a.x, a.z) > max(a.y, a.w);
        float4 blending_offset = float4(0.0f, a.y, 0.0f, a.w);
        float2 blending_weight = a.yw;
        smaa_movc4(bool4(h), blending_offset, float4(a.x, 0.0f, a.z, 0.0f));
        smaa_movc2(bool2(h), blending_weight, a.xz);
        blending_weight /= dot(blending_weight, float2(1.0f));

        const float4 coord = fma(blending_offset, float4(rt.x, rt.y, -rt.x, -rt.y), uv.xyxy);
        colour = blending_weight.x * source.sample(s, coord.xy, level(0.0f));
        colour += blending_weight.y * source.sample(s, coord.zw, level(0.0f));
    }

    if (source_is_linear != 0u) {
        colour.rgb = decode_srgb(colour.rgb);
    }
    return colour;
}

fragment float4 present_fragment(VertexOut in [[stage_in]],
                                 texture2d<float> source [[texture(0)]],
                                 sampler source_sampler [[sampler(0)]],
                                 constant uint& source_is_linear [[buffer(1)]]) {
    float4 colour = source.sample(source_sampler, in.uv);

    // See decode_srgb. An sRGB source never takes this branch: its sample
    // already decoded to linear.
    if (source_is_linear != 0u) {
        colour.rgb = decode_srgb(colour.rgb);
    }
    return colour;
}

// --------------------------------------------------------------------------
// Resampling filters.
//
// Each one reconstructs the source at the sample position from several texels
// rather than leaning on the sampler, so they all share one signature and the
// pipeline differs only in which fragment function it names. Weighting is done
// on decoded values - blending sRGB-encoded numbers is the classic
// gamma-incorrect resample, and it shows up as haloing on high-contrast edges.
//
// params.xy carries the cubic B and C; params.zw is how many source texels one
// output pixel covers, which only the area filter needs.
// --------------------------------------------------------------------------

// Mitchell-Netravali. B and C pick the family member: (0, 0.5) is Catmull-Rom,
// (1/3, 1/3) the Mitchell filter, (1, 0) the B-spline, (0, 0) zero-tangent.
inline float cubic_weight(float x, float B, float C) {
    x = abs(x);
    const float x2 = x * x;
    const float x3 = x2 * x;
    if (x < 1.0f) {
        return ((12.0f - 9.0f * B - 6.0f * C) * x3 +
                (-18.0f + 12.0f * B + 6.0f * C) * x2 + (6.0f - 2.0f * B)) / 6.0f;
    }
    if (x < 2.0f) {
        return ((-B - 6.0f * C) * x3 + (6.0f * B + 30.0f * C) * x2 +
                (-12.0f * B - 48.0f * C) * x + (8.0f * B + 24.0f * C)) / 6.0f;
    }
    return 0.0f;
}

fragment float4 present_cubic(VertexOut in [[stage_in]],
                              texture2d<float> source [[texture(0)]],
                              sampler source_sampler [[sampler(0)]],
                              constant uint& source_is_linear [[buffer(1)]],
                              constant float4& params [[buffer(2)]]) {
    const float2 size = float2(source.get_width(), source.get_height());
    const float2 coord = in.uv * size - 0.5f;
    const float2 base = floor(coord);
    const float2 frac = coord - base;

    float4 sum = float4(0.0f);
    float total = 0.0f;
    for (int j = -1; j <= 2; ++j) {
        const float wy = cubic_weight(float(j) - frac.y, params.x, params.y);
        for (int i = -1; i <= 2; ++i) {
            const float w = cubic_weight(float(i) - frac.x, params.x, params.y) * wy;
            const float2 uv = (base + float2(float(i), float(j)) + 0.5f) / size;
            sum += fetch_texel(source, source_sampler, uv, source_is_linear) * w;
            total += w;
        }
    }
    // A cubic kernel can sum to zero at the edge of its support; fall back
    // rather than divide by it.
    return total != 0.0f ? sum / total
                         : fetch_texel(source, source_sampler, in.uv, source_is_linear);
}

inline float lanczos_weight(float x, float a) {
    x = abs(x);
    if (x < 1.0e-5f) {
        return 1.0f;
    }
    if (x >= a) {
        return 0.0f;
    }
    const float pix = M_PI_F * x;
    return (a * sin(pix) * sin(pix / a)) / (pix * pix);
}

fragment float4 present_lanczos(VertexOut in [[stage_in]],
                                texture2d<float> source [[texture(0)]],
                                sampler source_sampler [[sampler(0)]],
                                constant uint& source_is_linear [[buffer(1)]],
                                constant float4& params [[buffer(2)]]) {
    const float2 size = float2(source.get_width(), source.get_height());
    const float2 coord = in.uv * size - 0.5f;
    const float2 base = floor(coord);
    const float2 frac = coord - base;

    float4 sum = float4(0.0f);
    float total = 0.0f;
    for (int j = -1; j <= 2; ++j) {
        const float wy = lanczos_weight(float(j) - frac.y, 2.0f);
        for (int i = -1; i <= 2; ++i) {
            const float w = lanczos_weight(float(i) - frac.x, 2.0f) * wy;
            const float2 uv = (base + float2(float(i), float(j)) + 0.5f) / size;
            sum += fetch_texel(source, source_sampler, uv, source_is_linear) * w;
            total += w;
        }
    }
    return total != 0.0f ? sum / total
                         : fetch_texel(source, source_sampler, in.uv, source_is_linear);
}

fragment float4 present_gaussian(VertexOut in [[stage_in]],
                                 texture2d<float> source [[texture(0)]],
                                 sampler source_sampler [[sampler(0)]],
                                 constant uint& source_is_linear [[buffer(1)]],
                                 constant float4& params [[buffer(2)]]) {
    const float2 size = float2(source.get_width(), source.get_height());
    // Modest by design: a wide kernel at 1x output is just a blur.
    const float sigma = 0.75f;
    const float denom = 2.0f * sigma * sigma;

    float4 sum = float4(0.0f);
    float total = 0.0f;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            const float2 offset = float2(float(i), float(j));
            const float w = exp(-dot(offset, offset) / denom);
            const float2 uv = in.uv + offset / size;
            sum += fetch_texel(source, source_sampler, uv, source_is_linear) * w;
            total += w;
        }
    }
    return sum / total;
}

// --------------------------------------------------------------------------
// Scaleforce.
//
// Ported from BreadFish64's ScaleFish (MIT), by way of the OpenGL backend's
// opengl_present_scaleforce.frag. Single pass and self-contained.
//
// The original's textureOffset y runs opposite to Metal's, and it derives a
// *direction* from its neighbourhood rather than just averaging it - so the
// whole computation stays in the original's frame (y negated going in and
// coming out) instead of flipping only the taps, which would smear the wrong
// way on horizontal edges. The original's fp16 path is dropped; on Apple GPUs
// the conversions cost more than the math saves.
// --------------------------------------------------------------------------

inline float scaleforce_dist1(float4 a, float4 b) {
    // BT.2020 luma/chroma, per the original.
    const float3 K = float3(0.2627f, 0.6780f, 0.0593f);
    const float scaleB = 0.5f / (1.0f - K.b);
    const float scaleR = 0.5f / (1.0f - K.r);
    const float4 diff = a - b;
    const float Y = dot(diff.rgb, K);
    const float Cb = scaleB * (diff.b - Y);
    const float Cr = scaleR * (diff.r - Y);
    // ignore_alpha is true in the original, so alpha never enters the distance.
    return length(float3(Y, Cb, Cr));
}

inline float4 scaleforce_dist(float4 ref, float4 A, float4 B, float4 C, float4 D) {
    return float4(scaleforce_dist1(ref, A), scaleforce_dist1(ref, B),
                  scaleforce_dist1(ref, C), scaleforce_dist1(ref, D));
}

fragment float4 present_scaleforce(VertexOut in [[stage_in]],
                                   texture2d<float> source [[texture(0)]],
                                   sampler source_sampler [[sampler(0)]],
                                   constant uint& source_is_linear [[buffer(1)]],
                                   constant float4& params [[buffer(2)]]) {
    // y negated so these keep the original's meaning: see the note above.
    const float4 bl = source.sample(source_sampler, in.uv, int2(-1,  1));
    const float4 bc = source.sample(source_sampler, in.uv, int2( 0,  1));
    const float4 br = source.sample(source_sampler, in.uv, int2( 1,  1));
    const float4 cl = source.sample(source_sampler, in.uv, int2(-1,  0));
    const float4 cc = source.sample(source_sampler, in.uv);
    const float4 cr = source.sample(source_sampler, in.uv, int2( 1,  0));
    const float4 tl = source.sample(source_sampler, in.uv, int2(-1, -1));
    const float4 tc = source.sample(source_sampler, in.uv, int2( 0, -1));
    const float4 tr = source.sample(source_sampler, in.uv, int2( 1, -1));

    const float4 offset_tl = scaleforce_dist(cc, tl, tc, tr, cr);
    const float4 offset_br = scaleforce_dist(cc, br, bc, bl, cl);

    const float plus_weight = 1.5f;
    const float cross_weight = 1.5f;
    const float total_dist = dot(offset_tl + offset_br,
                                 float4(cross_weight, plus_weight, cross_weight, plus_weight));

    float4 colour = cc;
    if (total_dist != 0.0f) {
        const float4 tmp = offset_tl - offset_br;
        const float2 total_offset =
            tmp.wy * plus_weight + (tmp.zz + float2(-tmp.x, tmp.x)) * cross_weight;

        // Thin features split apart when the offset reaches into clear areas,
        // so it is clamped by its own magnitude over the total distance.
        const float clamp_val = length(total_offset) / total_dist;
        const float2 size = float2(source.get_width(), source.get_height());
        const float2 final_offset =
            clamp(total_offset, float2(-clamp_val), float2(clamp_val)) / size;

        // Back out of the original's frame: y negated again.
        colour = source.sample(source_sampler,
                               in.uv - float2(final_offset.x, -final_offset.y));
    }

    if (source_is_linear != 0u) {
        colour.rgb = decode_srgb(colour.rgb);
    }
    return colour;
}

// --------------------------------------------------------------------------
// FXAA.
//
// Runs before scaling, at source resolution, writing into a texture of the
// source's own format - so the present pass that follows treats its output
// exactly as it would have treated the source, and the user's scaling filter
// still applies on top.
//
// The shader applies no decode of its own and the round trip is exact either
// way, so the space it weighs luma in follows the source format: linear for an
// sRGB source, gamma-encoded otherwise. Edge detection therefore differs
// slightly between the two.
// --------------------------------------------------------------------------

inline float fxaa_luma(float3 c) {
    return dot(c, float3(0.299f, 0.587f, 0.114f));
}

fragment float4 present_fxaa(VertexOut in [[stage_in]],
                             texture2d<float> source [[texture(0)]],
                             sampler source_sampler [[sampler(0)]],
                             constant uint& source_is_linear [[buffer(1)]],
                             constant float4& params [[buffer(2)]]) {
    const float EDGE_THRESHOLD_MIN = 1.0f / 128.0f;
    const float EDGE_THRESHOLD_MUL = 1.0f / 8.0f;
    const float SPAN_MAX = 8.0f;

    const float2 inv = 1.0f / float2(source.get_width(), source.get_height());

    const float3 rgbM = source.sample(source_sampler, in.uv).rgb;
    const float3 rgbNW = source.sample(source_sampler, in.uv + float2(-1.0f, -1.0f) * inv).rgb;
    const float3 rgbNE = source.sample(source_sampler, in.uv + float2( 1.0f, -1.0f) * inv).rgb;
    const float3 rgbSW = source.sample(source_sampler, in.uv + float2(-1.0f,  1.0f) * inv).rgb;
    const float3 rgbSE = source.sample(source_sampler, in.uv + float2( 1.0f,  1.0f) * inv).rgb;

    const float lumaM = fxaa_luma(rgbM);
    const float lumaNW = fxaa_luma(rgbNW);
    const float lumaNE = fxaa_luma(rgbNE);
    const float lumaSW = fxaa_luma(rgbSW);
    const float lumaSE = fxaa_luma(rgbSE);

    const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    // A flat neighbourhood has no edge to soften, and blurring it is the
    // artefact FXAA is most often blamed for.
    if ((lumaMax - lumaMin) < max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD_MUL)) {
        return float4(rgbM, 1.0f);
    }

    // Edge normal, from the luma gradient across the four diagonals.
    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    // Bias short gradients away from zero so a near-axis-aligned edge does not
    // produce an enormous step.
    const float reduce =
        max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.25f * EDGE_THRESHOLD_MUL, EDGE_THRESHOLD_MIN);
    const float rcp_min = 1.0f / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp_min, float2(-SPAN_MAX), float2(SPAN_MAX)) * inv;

    // Two taps across the edge, then two wider ones. The wide pair is used only
    // when it stays within the neighbourhood's luma range; outside it, it has
    // reached past the edge and would smear.
    const float3 rgbA = 0.5f * (source.sample(source_sampler, in.uv + dir * (1.0f / 3.0f - 0.5f)).rgb +
                                source.sample(source_sampler, in.uv + dir * (2.0f / 3.0f - 0.5f)).rgb);
    const float3 rgbB = rgbA * 0.5f +
                        0.25f * (source.sample(source_sampler, in.uv - dir * 0.5f).rgb +
                                 source.sample(source_sampler, in.uv + dir * 0.5f).rgb);

    const float lumaB = fxaa_luma(rgbB);
    return float4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0f);
}

fragment float4 present_area(VertexOut in [[stage_in]],
                             texture2d<float> source [[texture(0)]],
                             sampler source_sampler [[sampler(0)]],
                             constant uint& source_is_linear [[buffer(1)]],
                             constant float4& params [[buffer(2)]]) {
    const float2 size = float2(source.get_width(), source.get_height());
    // How much of the source one output pixel covers. Below one texel there is
    // nothing to average and this degenerates to a plain sample, which is the
    // right answer when upscaling - area resampling is a downscale filter.
    const float2 footprint = max(params.zw, float2(1.0f));
    const int2 taps = min(int2(ceil(footprint)), int2(8));

    float4 sum = float4(0.0f);
    float total = 0.0f;
    for (int j = 0; j < taps.y; ++j) {
        for (int i = 0; i < taps.x; ++i) {
            // Sample centres spread evenly across the footprint.
            const float2 centre = (float2(float(i), float(j)) + 0.5f) / float2(taps);
            const float2 uv = in.uv + (centre - 0.5f) * footprint / size;
            sum += fetch_texel(source, source_sampler, uv, source_is_linear);
            total += 1.0f;
        }
    }
    return sum / total;
}
)MSL";

// Whether sampling @p format applies the sRGB -> linear decode in the texture
// unit. Only the four uncompressed 8-bit formats can reach the presentation
// blit, so this is an exact list instead of a guess at a naming rule.
bool IsSrgbFormat(MTL::PixelFormat format) {
    switch (format) {
    case MTL::PixelFormatRGBA8Unorm_sRGB:
    case MTL::PixelFormatBGRA8Unorm_sRGB:
    case MTL::PixelFormatR8Unorm_sRGB:
    case MTL::PixelFormatRG8Unorm_sRGB:
        return true;
    default:
        return false;
    }
}

Owned<MTL::SamplerState> MakeSampler(const Device& device, MTL::SamplerMinMagFilter filter) {
    ScopedAutoreleasePool pool;

    MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();
    descriptor->setMinFilter(filter);
    descriptor->setMagFilter(filter);
    descriptor->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
    // Clamp, sampling outside the crop rectangle must not wrap the image.
    descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);

    Owned<MTL::SamplerState> sampler = Adopt(device.GetDevice()->newSamplerState(descriptor));
    descriptor->release();
    return sampler;
}

// Fragment function for a filter kind. The names have to match the MSL.
const char* FragmentNameOf(BlitScreen::Filter filter) {
    switch (filter) {
    case BlitScreen::Filter::Cubic:
        return "present_cubic";
    case BlitScreen::Filter::Gaussian:
        return "present_gaussian";
    case BlitScreen::Filter::Lanczos:
        return "present_lanczos";
    case BlitScreen::Filter::Area:
        return "present_area";
    case BlitScreen::Filter::Fxaa:
        return "present_fxaa";
    case BlitScreen::Filter::ScaleForce:
        return "present_scaleforce";
    case BlitScreen::Filter::Spline1:
        return "present_spline1";
    case BlitScreen::Filter::Mmpx:
        return "present_mmpx";
    case BlitScreen::Filter::Sgsr:
        return "present_sgsr";
    case BlitScreen::Filter::SgsrEdge:
        return "present_sgsr_edge";
    case BlitScreen::Filter::FsrEasu:
        return "present_fsr_easu";
    case BlitScreen::Filter::FsrRcas:
        return "present_fsr_rcas";
    case BlitScreen::Filter::SmaaEdges:
        return "present_smaa_edges";
    case BlitScreen::Filter::SmaaWeights:
        return "present_smaa_weights";
    case BlitScreen::Filter::SmaaBlend:
        return "present_smaa_blend";
    case BlitScreen::Filter::Standard:
        break;
    }
    return "present_fragment";
}

// Maps the user's setting onto a fragment program and its parameters. The cubic
// filters differ only in their B and C so they share one shader. Settings with
// no entry (nearest, bilinear, MetalFx) fall to the plain sampled path plus
// sampler state, and Draw handles Fsr itself.
struct FilterChoice {
    BlitScreen::Filter filter;
    f32 b;
    f32 c;
};

FilterChoice ChooseFilter(ScalingFilter setting) {
    using Setting = ScalingFilter;
    using Filter = BlitScreen::Filter;
    switch (setting) {
    case Setting::Bicubic:
    case Setting::CatmullRom:
    case Setting::CatmullRomFast:
    case Setting::CasCatmullRom:
    case Setting::CasCatmullRomFast:
        // Catmull-Rom, the interpolating one, and what "bicubic" means here.
        return {Filter::Cubic, 0.0f, 0.5f};
    case Setting::Gaussian:
        return {Filter::Gaussian, 0.0f, 0.0f};
    case Setting::Area:
        return {Filter::Area, 0.0f, 0.0f};
    case Setting::ScaleForce:
    case Setting::ScaleForceFast:
        return {Filter::ScaleForce, 0.0f, 0.0f};
    // Replaced wholesale by the two-pass path in Draw.
    case Setting::Fsr:
    case Setting::FsrEasu:
        return {Filter::Standard, 0.0f, 0.0f};
    default:
        return {Filter::Standard, 0.0f, 0.0f};
    }
}

} // Anonymous namespace

BlitScreen::BlitScreen(const Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_} {
    ScopedAutoreleasePool pool;

    NS::Error* error = nullptr;
    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
    library = Adopt(device.GetDevice()->newLibrary(ToNSString(PRESENT_MSL), options, &error));
    options->release();

    if (!library) {
        LOG_ERROR(Render_Metal, "Failed to compile the present shader library: {}",
                  error != nullptr ? error->localizedDescription()->utf8String() : "no error");
    }

    // Both up front, the filter can change at any time and a sampler is cheap.
    sampler_linear = MakeSampler(device, MTL::SamplerMinMagFilterLinear);
    sampler_nearest = MakeSampler(device, MTL::SamplerMinMagFilterNearest);
}

BlitScreen::~BlitScreen() = default;

MTL::RenderPipelineState* BlitScreen::GetPipeline(MTL::PixelFormat target_format, Filter filter,
                                                  Tegra::BlendMode blending) {
    if (!library) {
        return nullptr;
    }
    const u64 key = (static_cast<u64>(target_format) << 32) |
                    (static_cast<u64>(blending) << 16) | static_cast<u64>(filter);
    if (const auto it = pipelines.find(key); it != pipelines.end()) {
        // Cached nulls included. A filter whose pipeline failed once fails every
        // frame, and retrying would recompile on every present.
        return it->second.get();
    }
    ScopedAutoreleasePool pool;

    MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
    descriptor->setVertexFunctionDescriptor(
        MakeFunctionDescriptor(library.get(), "present_vertex"));
    descriptor->setFragmentFunctionDescriptor(
        MakeFunctionDescriptor(library.get(), FragmentNameOf(filter)));

    MTL4::RenderPipelineColorAttachmentDescriptor* attachment =
        MTL4::RenderPipelineColorAttachmentDescriptor::alloc()->init();
    attachment->setPixelFormat(target_format);

    // Overlay layers (applet UI) carry a guest-chosen blend mode and have to be
    // combined with the game frame underneath. These factors mirror the Vulkan
    // present pipelines, see CreateWrappedPremultipliedBlendingPipeline.
    if (blending != Tegra::BlendMode::Opaque) {
        attachment->setBlendingState(MTL4::BlendStateEnabled);
        attachment->setRgbBlendOperation(MTL::BlendOperationAdd);
        attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
        // Premultiplied layers already had alpha folded into the colour so we
        // take the source as-is, coverage layers still need scaling by it. Both
        // leave the destination alpha at the source's, which is what an opaque
        // drawable expects.
        attachment->setSourceRGBBlendFactor(blending == Tegra::BlendMode::Premultiplied
                                                ? MTL::BlendFactorOne
                                                : MTL::BlendFactorSourceAlpha);
        attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
        attachment->setSourceAlphaBlendFactor(MTL::BlendFactorOne);
        attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorZero);
    }
    // No vertex descriptor, positions come from the vertex id.
    descriptor->colorAttachments()->setObject(attachment, 0);
    attachment->release();

    NS::Error* error = nullptr;
    Owned<MTL::RenderPipelineState> pipeline =
        device.NewRenderPipelineState(descriptor, &error);
    descriptor->release();

    if (!pipeline) {
        LOG_ERROR(Render_Metal, "Failed to create the {} present pipeline: {}",
                  FragmentNameOf(filter), DescribeError(error));
    } else {
        LOG_INFO(Render_Metal, "Present pipeline ready ({})", FragmentNameOf(filter));
    }
    return pipelines.emplace(key, std::move(pipeline)).first->second.get();
}

void BlitScreen::EnsureSmaaLuts() {
    if (smaa_area_lut && smaa_search_lut) {
        return;
    }
    ScopedAutoreleasePool pool;

    const auto make_lut = [this](MTL::PixelFormat format, u32 width, u32 height,
                                 const void* data, u32 bytes_per_row) -> Owned<MTL::Texture> {
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(MTL::TextureType2D);
        descriptor->setPixelFormat(format);
        descriptor->setWidth(width);
        descriptor->setHeight(height);
        descriptor->setMipmapLevelCount(1);
        descriptor->setUsage(MTL::TextureUsageShaderRead);
        // Shared, written once from the CPU and only sampled afterwards.
        descriptor->setStorageMode(MTL::StorageModeShared);
        Owned<MTL::Texture> texture = Adopt(device.GetDevice()->newTexture(descriptor));
        descriptor->release();
        if (texture) {
            device.AddResidentAllocation(texture.get());
            device.CommitResidency();
            texture->replaceRegion(MTL::Region::Make2D(0, 0, width, height), 0, data,
                                   bytes_per_row);
        }
        return texture;
    };

    // The reference stores both tables bottom-up, so upload them in that same
    // order. Flipping here would break how the shader samples them.
    smaa_area_lut = make_lut(MTL::PixelFormatRG8Unorm, AREATEX_WIDTH, AREATEX_HEIGHT,
                             areaTexBytes, AREATEX_PITCH);
    smaa_search_lut = make_lut(MTL::PixelFormatR8Unorm, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
                               searchTexBytes, SEARCHTEX_PITCH);
    if (!smaa_area_lut || !smaa_search_lut) {
        LOG_ERROR(Render_Metal, "Failed to allocate the SMAA lookup textures");
    }
}

MTL::Texture* BlitScreen::SmaaAntiAlias(MTL::Texture* source) {
    if (source == nullptr) {
        return source;
    }
    const u32 width = static_cast<u32>(source->width());
    const u32 height = static_cast<u32>(source->height());
    if (width == 0 || height == 0) {
        return source;
    }

    EnsureSmaaLuts();
    if (!smaa_area_lut || !smaa_search_lut) {
        return source;
    }

    if (!smaa_edges || !smaa_output || smaa_width != width || smaa_height != height ||
        smaa_output->pixelFormat() != source->pixelFormat()) {
        ScopedAutoreleasePool pool;
        const auto make_target = [this, width, height](MTL::PixelFormat format,
                                                       const char* label) {
            MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
            descriptor->setTextureType(MTL::TextureType2D);
            descriptor->setPixelFormat(format);
            descriptor->setWidth(width);
            descriptor->setHeight(height);
            descriptor->setMipmapLevelCount(1);
            descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            descriptor->setStorageMode(MTL::StorageModePrivate);
            Owned<MTL::Texture> texture = Adopt(device.GetDevice()->newTexture(descriptor));
            descriptor->release();
            if (texture) {
                texture->setLabel(ToNSString(label));
                device.AddResidentAllocation(texture.get());
                device.CommitResidency();
            }
            return texture;
        };
        // Has to outlive the recorded frames that may still sample it, see
        // Scheduler::KeepAlive.
        if (smaa_edges) {
            scheduler.KeepAlive(smaa_edges.get(), smaa_edges.get());
        }
        if (smaa_weights) {
            scheduler.KeepAlive(smaa_weights.get(), smaa_weights.get());
        }
        if (smaa_output) {
            scheduler.KeepAlive(smaa_output.get(), smaa_output.get());
        }
        smaa_edges = make_target(MTL::PixelFormatRG8Unorm, "eden SMAA edges");
        smaa_weights = make_target(MTL::PixelFormatRGBA8Unorm, "eden SMAA weights");
        // The blend pass can't write into the texture it samples, so its result
        // goes to a third target instead of back over the source.
        smaa_output = make_target(source->pixelFormat(), "eden SMAA output");
        if (!smaa_edges || !smaa_weights || !smaa_output) {
            LOG_ERROR(Render_Metal, "Failed to allocate the {}x{} SMAA targets", width, height);
            if (smaa_edges) {
                device.RemoveResidentAllocation(smaa_edges.get());
            }
            if (smaa_weights) {
                device.RemoveResidentAllocation(smaa_weights.get());
            }
            if (smaa_output) {
                device.RemoveResidentAllocation(smaa_output.get());
            }
            device.CommitResidency();
            smaa_edges = {};
            smaa_weights = {};
            smaa_output = {};
            smaa_width = smaa_height = 0;
            return source;
        }
        smaa_width = width;
        smaa_height = height;
    }

    MTL::RenderPipelineState* edge_pipeline =
        GetPipeline(MTL::PixelFormatRG8Unorm, Filter::SmaaEdges);
    MTL::RenderPipelineState* weight_pipeline =
        GetPipeline(MTL::PixelFormatRGBA8Unorm, Filter::SmaaWeights);
    MTL::RenderPipelineState* blend_pipeline =
        GetPipeline(source->pixelFormat(), Filter::SmaaBlend);
    if (edge_pipeline == nullptr || weight_pipeline == nullptr || blend_pipeline == nullptr) {
        return source;
    }

    // Full-frame passes. The present pass applies the crop later, so all three
    // of these cover the whole intermediate.
    const std::array<f32, 4> crop{0.0f, 0.0f, 1.0f, 1.0f};
    MTL::SamplerState* const sampler = sampler_linear.get();

    const auto run_pass = [&](MTL::Texture* target, MTL::RenderPipelineState* pipeline,
                              MTL::Texture* tex0, MTL::Texture* tex1, MTL::Texture* tex2,
                              bool clear, const u32* is_linear) {
        RenderPassState pass{};
        pass.num_color = 1;
        pass.color[0].texture = target;
        // Edge and weight targets get read where nothing was written, so clear
        // them instead of leaving them undefined. The blend pass covers every
        // pixel and doesn't need it.
        pass.color[0].load_action = clear ? LoadAction::Clear : LoadAction::DontCare;
        pass.color[0].clear_color = {0.0, 0.0, 0.0, 0.0};
        pass.color[0].store_action = StoreAction::Store;
        pass.render_area_width = width;
        pass.render_area_height = height;

        const u32 linear_value = is_linear != nullptr ? *is_linear : 0u;
        const bool has_linear = is_linear != nullptr;

        scheduler.RequestRenderPass(pass);
        scheduler.Record([pipeline, tex0, tex1, tex2, sampler, crop, width, height, linear_value,
                          has_linear](CommandContext& context) {
            RenderEncoder encoder = context.CurrentRender();
            if (encoder == nullptr) {
                return;
            }
            // Binds straight onto the open encoder and skips RenderCache(), so
            // the draw path's idea of what's bound has to be thrown away.
            context.InvalidateRenderCache();
            MTL::Viewport viewport{};
            viewport.width = static_cast<double>(width);
            viewport.height = static_cast<double>(height);
            viewport.znear = 0.0;
            viewport.zfar = 1.0;
            encoder->setViewport(viewport);
            encoder->setRenderPipelineState(pipeline);
            encoder->setVertexBytes(crop.data(), crop.size() * sizeof(f32), 1);
            encoder->setFragmentSamplerState(sampler, 0);
            encoder->setFragmentTexture(tex0, 0);
            if (tex1 != nullptr) {
                encoder->setFragmentTexture(tex1, 1);
            }
            if (tex2 != nullptr) {
                encoder->setFragmentTexture(tex2, 2);
            }
            if (has_linear) {
                encoder->setFragmentBytes(&linear_value, sizeof(linear_value), 1);
            }
            encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{0}, NS::UInteger{3});
        });
        scheduler.RequestOutsideRenderPassOperationContext();
    };

    // No decode anywhere in the chain. SMAA runs at source resolution into a
    // target of the source's own format, so the present pass still sees the
    // encoding it would have seen without SMAA and does the decode.
    const u32 blend_is_linear = 0u;
    run_pass(smaa_edges.get(), edge_pipeline, source, nullptr, nullptr, true, nullptr);
    run_pass(smaa_weights.get(), weight_pipeline, smaa_edges.get(), smaa_area_lut.get(),
             smaa_search_lut.get(), true, nullptr);
    run_pass(smaa_output.get(), blend_pipeline, source, smaa_weights.get(), nullptr, false,
             &blend_is_linear);

    return smaa_output.get();
}

MTL::Texture* BlitScreen::MetalFxUpscale(MTL::Texture* source, u32 target_width,
                                         u32 target_height, f32 crop_left, f32 crop_top,
                                         f32 crop_right, f32 crop_bottom) {
    if (source == nullptr || target_width == 0 || target_height == 0) {
        return source;
    }
    // We can only express the crop's extent, the scaler takes an input content
    // *size* and not a rectangle, so a crop that doesn't start at the top left
    // would scale the wrong region. The guest's usual case trims padding on the
    // right and bottom, which this does cover.
    if (crop_left != 0.0f || crop_top != 0.0f) {
        return source;
    }

    const u32 source_width = static_cast<u32>(source->width());
    const u32 source_height = static_cast<u32>(source->height());
    const u32 content_width =
        std::max(1u, static_cast<u32>(static_cast<f32>(source_width) * crop_right));
    const u32 content_height =
        std::max(1u, static_cast<u32>(static_cast<f32>(source_height) * crop_bottom));

    // Nothing to reconstruct when the frame isn't actually being enlarged, and
    // MetalFX is a spatial *upscaler*. Handing it a downscale is wasteful and
    // worse than a plain filtered blit.
    if (content_width >= target_width && content_height >= target_height) {
        return source;
    }

    const MTL::PixelFormat source_format = source->pixelFormat();
    // The content size is part of the key and not just per-frame state. The
    // scaler keeps upscaling the region it was first encoded with, so a
    // docked/handheld switch, which narrows the crop instead of resizing the
    // guest's buffer, would come out magnified and cropped at the window edges.
    const bool needs_rebuild =
        !metalfx_scaler_mtl4 || metalfx_input_width != source_width ||
        metalfx_input_height != source_height || metalfx_output_width != target_width ||
        metalfx_output_height != target_height || metalfx_input_format != source_format ||
        metalfx_content_width != content_width || metalfx_content_height != content_height;

    if (needs_rebuild) {
        ScopedAutoreleasePool pool;
        // Metal 4 command buffers don't retain the resources their commands
        // touch, so dropping the last reference to a texture an already-recorded
        // frame still samples frees it out from under the worker. See KeepAlive.
        if (metalfx_output) {
            scheduler.KeepAlive(metalfx_output.get(), metalfx_output.get());
        }
        metalfx_scaler_mtl4.reset();
        metalfx_output.reset();

        if (!MTLFX::SpatialScalerDescriptor::supportsDevice(device.GetDevice())) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG_WARNING(Render_Metal, "MetalFX is unavailable on this device");
            }
            return source;
        }

        MTLFX::SpatialScalerDescriptor* descriptor =
            MTLFX::SpatialScalerDescriptor::alloc()->init();
        descriptor->setColorTextureFormat(source_format);
        descriptor->setOutputTextureFormat(source_format);
        descriptor->setInputWidth(source_width);
        descriptor->setInputHeight(source_height);
        descriptor->setOutputWidth(target_width);
        descriptor->setOutputHeight(target_height);
        // Perceptual, the guest's framebuffer holds display-referred values and
        // not linear light. Tell MetalFX otherwise and it reconstructs against
        // the wrong transfer curve, which shows up as haloing on high contrast.
        descriptor->setColorProcessingMode(
            MTLFX::SpatialScalerColorProcessingModePerceptual);

        metalfx_scaler_mtl4 =
            Adopt(descriptor->newSpatialScaler(device.GetDevice(),
                                               device.GetMTL4UtilityCompiler()));
        descriptor->release();

        if (!metalfx_scaler_mtl4) {
            LOG_ERROR(Render_Metal, "Failed to create the MetalFX spatial scaler");
            return source;
        }

        MTL::TextureDescriptor* output_descriptor = MTL::TextureDescriptor::alloc()->init();
        output_descriptor->setTextureType(MTL::TextureType2D);
        output_descriptor->setPixelFormat(source_format);
        output_descriptor->setWidth(target_width);
        output_descriptor->setHeight(target_height);
        output_descriptor->setMipmapLevelCount(1);
        // MetalFX writes it, the present pass samples it.
        output_descriptor->setUsage(MTL::TextureUsageRenderTarget |
                                    MTL::TextureUsageShaderRead |
                                    MTL::TextureUsageShaderWrite);
        output_descriptor->setStorageMode(MTL::StorageModePrivate);
        metalfx_output = Adopt(device.GetDevice()->newTexture(output_descriptor));
        output_descriptor->release();
        if (!metalfx_output) {
            LOG_ERROR(Render_Metal, "Failed to allocate the {}x{} MetalFX target", target_width,
                      target_height);
            metalfx_scaler_mtl4.reset();
            return source;
        }
        metalfx_output->setLabel(MTLSTR("eden MetalFX output"));
        device.AddResidentAllocation(metalfx_output.get());
        // Committed right away. Additions don't take effect until commit(), and
        // MetalFX encodes straight into the command buffer so nothing guarantees
        // a Flush before the GPU first reads this texture. A non-resident read is
        // unmapped memory with no validation error.
        device.CommitResidency();

        metalfx_input_width = source_width;
        metalfx_input_height = source_height;
        metalfx_output_width = target_width;
        metalfx_output_height = target_height;
        metalfx_input_format = source_format;
        metalfx_content_width = content_width;
        metalfx_content_height = content_height;
        LOG_INFO(Render_Metal, "MetalFX spatial: {}x{} (content {}x{}) -> {}x{}", source_width,
                 source_height, content_width, content_height, target_width, target_height);
    }

    // MetalFX encodes into the command buffer directly and not into an encoder,
    // so close every open encoder first.
    // RequestOutsideRenderPassOperationContext doesn't close compute or copy.
    scheduler.RequestCloseEncoders();

    MTL4FX::SpatialScaler* const scaler = metalfx_scaler_mtl4.get();
    MTL::Texture* const output = metalfx_output.get();
    scheduler.Record([scaler, source, output, content_width,
                      content_height](CommandContext& context) {
        scaler->setColorTexture(source);
        scaler->setOutputTexture(output);
        scaler->setInputContentWidth(content_width);
        scaler->setInputContentHeight(content_height);
        if (MTL4::CommandBuffer* const buffer = context.MTL4CommandBuffer()) {
            scaler->encodeToCommandBuffer(buffer);
        }
    });

    return output;
}

MTL::Texture* BlitScreen::FsrUpscale(MTL::Texture* source, u32 target_width, u32 target_height,
                                     f32 crop_left, f32 crop_top, f32 crop_right,
                                     f32 crop_bottom) {
    if (source == nullptr || target_width == 0 || target_height == 0) {
        return source;
    }

    // RGB10A2Unorm: 10-bit linear SDR precision (zero quantization on 8-bit
    // Switch output) at HALF the memory bandwidth & TBDR tile footprint of
    // RGBA16Float (32 bpp vs 64 bpp).
    constexpr MTL::PixelFormat FSR_FORMAT = MTL::PixelFormatRGB10A2Unorm;
    if (!fsr_texture || fsr_width != target_width || fsr_height != target_height) {
        ScopedAutoreleasePool pool;
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(MTL::TextureType2D);
        descriptor->setPixelFormat(FSR_FORMAT);
        descriptor->setWidth(target_width);
        descriptor->setHeight(target_height);
        descriptor->setMipmapLevelCount(1);
        descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        descriptor->setStorageMode(MTL::StorageModePrivate);
        // Has to outlive the recorded frames that may still sample it, see
        // Scheduler::KeepAlive.
        if (fsr_texture) {
            scheduler.KeepAlive(fsr_texture.get(), fsr_texture.get());
        }
        fsr_texture = Adopt(device.GetDevice()->newTexture(descriptor));
        descriptor->release();
        if (fsr_texture) {
            device.AddResidentAllocation(fsr_texture.get());
            device.CommitResidency();
        }
        if (!fsr_texture) {
            LOG_ERROR(Render_Metal, "Failed to allocate the {}x{} FSR target", target_width,
                      target_height);
            fsr_width = fsr_height = 0;
            return source;
        }
        fsr_texture->setLabel(MTLSTR("eden FSR EASU"));
        fsr_width = target_width;
        fsr_height = target_height;
    }

    MTL::RenderPipelineState* pipeline = GetPipeline(FSR_FORMAT, Filter::FsrEasu);
    if (pipeline == nullptr) {
        return source;
    }

    RenderPassState pass{};
    pass.num_color = 1;
    pass.color[0].texture = fsr_texture.get();
    pass.color[0].load_action = LoadAction::DontCare;
    pass.color[0].store_action = StoreAction::Store;
    pass.render_area_width = target_width;
    pass.render_area_height = target_height;

    const std::array<f32, 4> crop{crop_left, crop_top, crop_right, crop_bottom};
    // EASU reconstructs from the source's own texels, so it takes the linear
    // sampler for its gathers and does the sRGB decode in this pass. RCAS then
    // works on values that are already linear.
    const u32 source_is_linear = IsSrgbFormat(source->pixelFormat()) ? 0u : 1u;
    const std::array<f32, 4> params{0.0f, 0.0f, 0.0f, 0.0f};
    MTL::SamplerState* sampler = sampler_linear.get();
    MTL::Texture* const input = source;

    scheduler.RequestRenderPass(pass);
    scheduler.Record([pipeline, input, sampler, crop, params, source_is_linear, target_width,
                      target_height](CommandContext& context) {
        RenderEncoder encoder = context.CurrentRender();
        if (encoder == nullptr) {
            return;
        }
        // Binds straight onto the open encoder and skips RenderCache(), so the
        // draw path's idea of what's bound has to be thrown away.
        context.InvalidateRenderCache();
        MTL::Viewport viewport{};
        viewport.width = static_cast<double>(target_width);
        viewport.height = static_cast<double>(target_height);
        viewport.znear = 0.0;
        viewport.zfar = 1.0;
        encoder->setViewport(viewport);
        encoder->setRenderPipelineState(pipeline);
        encoder->setVertexBytes(crop.data(), crop.size() * sizeof(f32), 1);
        encoder->setFragmentTexture(input, 0);
        encoder->setFragmentSamplerState(sampler, 0);
        encoder->setFragmentBytes(&source_is_linear, sizeof(source_is_linear), 1);
        encoder->setFragmentBytes(params.data(), params.size() * sizeof(f32), 2);
        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{0}, NS::UInteger{3});
    });
    scheduler.RequestOutsideRenderPassOperationContext();

    return fsr_texture.get();
}

MTL::Texture* BlitScreen::AntiAlias(MTL::Texture* source) {
    if (GetAntiAliasing() != AntiAliasing::Fxaa) {
        return source;
    }

    const u32 width = static_cast<u32>(source->width());
    const u32 height = static_cast<u32>(source->height());
    const MTL::PixelFormat format = source->pixelFormat();
    if (width == 0 || height == 0) {
        return source;
    }

    if (!aa_texture || aa_width != width || aa_height != height || aa_format != format) {
        ScopedAutoreleasePool pool;
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(MTL::TextureType2D);
        // The source's own format, so the present pass after it doesn't need to
        // know whether AA ran.
        descriptor->setPixelFormat(format);
        descriptor->setWidth(width);
        descriptor->setHeight(height);
        descriptor->setMipmapLevelCount(1);
        descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        descriptor->setStorageMode(MTL::StorageModePrivate);
        // Has to outlive the recorded frames that may still sample it, see
        // Scheduler::KeepAlive.
        if (aa_texture) {
            scheduler.KeepAlive(aa_texture.get(), aa_texture.get());
        }
        aa_texture = Adopt(device.GetDevice()->newTexture(descriptor));
        descriptor->release();
        if (aa_texture) {
            device.AddResidentAllocation(aa_texture.get());
            device.CommitResidency();
        }

        if (!aa_texture) {
            // Not fatal and not worth a frame, present the source unfiltered.
            static std::atomic<u32> reported{0};
            if (reported++ < 4) {
                LOG_WARNING(Render_Metal, "Could not allocate a {}x{} anti-aliasing target",
                            width, height);
            }
            aa_width = 0;
            aa_height = 0;
            return source;
        }
        aa_texture->setLabel(MTLSTR("eden fxaa"));
        aa_width = width;
        aa_height = height;
        aa_format = format;
    }

    MTL::RenderPipelineState* pipeline = GetPipeline(format, Filter::Fxaa);
    if (pipeline == nullptr) {
        return source;
    }

    RenderPassState pass{};
    pass.num_color = 1;
    pass.color[0].texture = aa_texture.get();
    // Every pixel is written, so loading the previous frame is wasted bandwidth
    // on exactly the hardware that charges most for it.
    pass.color[0].load_action = LoadAction::DontCare;
    pass.color[0].store_action = StoreAction::Store;
    pass.render_area_width = width;
    pass.render_area_height = height;

    // The whole source, uncropped. Cropping is the present pass's job, doing it
    // here would anti-alias one region and then crop a different one.
    const std::array<f32, 4> crop{0.0f, 0.0f, 1.0f, 1.0f};
    const std::array<f32, 4> filter_params{0.0f, 0.0f, 1.0f, 1.0f};
    // No decode, see the shader. Bound because the entry point declares it.
    const u32 source_is_linear = 0u;
    MTL::SamplerState* sampler = sampler_linear.get();
    MTL::Texture* const result = aa_texture.get();

    // Opened on the worker instead of through Scheduler::RequestRenderPass, so
    // the recording-side mirror has to be told or every later request to end the
    // pass records nothing.
    scheduler.NotifyRenderPassOpened(pass);
    scheduler.Record([pipeline, source, sampler, crop, filter_params, source_is_linear, width,
                      height, pass](CommandContext& context) {
        RenderEncoder encoder = context.RequestRender(pass);
        if (encoder == nullptr) {
            return;
        }

        MTL::Viewport viewport{};
        viewport.originX = 0.0;
        viewport.originY = 0.0;
        viewport.width = static_cast<double>(width);
        viewport.height = static_cast<double>(height);
        viewport.znear = 0.0;
        viewport.zfar = 1.0;
        encoder->setViewport(viewport);

        encoder->setRenderPipelineState(pipeline);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setVertexBytes(crop.data(), crop.size() * sizeof(f32), CROP_BUFFER_INDEX);
        encoder->setFragmentTexture(source, SOURCE_TEXTURE_INDEX);
        encoder->setFragmentSamplerState(sampler, SOURCE_SAMPLER_INDEX);
        encoder->setFragmentBytes(&source_is_linear, sizeof(source_is_linear),
                                  SOURCE_IS_LINEAR_INDEX);
        encoder->setFragmentBytes(filter_params.data(), filter_params.size() * sizeof(f32),
                                  FILTER_PARAMS_INDEX);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{0}, NS::UInteger{3});
    });

    return result;
}

void BlitScreen::Draw(MTL::Texture* source, MTL::Texture* target, u32 target_width,
                      u32 target_height, f32 crop_left, f32 crop_top, f32 crop_right,
                      f32 crop_bottom, Tegra::BlendMode blending, bool is_base_layer,
                      const Common::Rectangle<u32>& screen, bool apply_post_processing) {
    if (source == nullptr || target == nullptr) {
        return;
    }

    // The frame goes at the layout's screen rectangle, which isn't the whole
    // drawable whenever the window's aspect ratio differs from the guest's.
    // Only trusted when it describes a rectangle inside this drawable.
    const bool screen_usable =
        screen.GetWidth() != 0 && screen.GetHeight() != 0 &&
        static_cast<u64>(screen.left) + screen.GetWidth() <= target_width &&
        static_cast<u64>(screen.top) + screen.GetHeight() <= target_height;
    const u32 draw_width = screen_usable ? screen.GetWidth() : target_width;
    const u32 draw_height = screen_usable ? screen.GetHeight() : target_height;

    const bool run_post = is_base_layer && apply_post_processing;
    const ScalingFilter requested_filter = GetScalingFilter();
    const Common::Rectangle<f32> crop_rect{crop_left, crop_top, crop_right, crop_bottom};
    const auto resolved_scale =
        run_post ? ResolvePresentScale(requested_filter, GetDownscalingFilter() != 0,
                                       static_cast<u32>(source->width()),
                                       static_cast<u32>(source->height()), crop_rect, draw_width,
                                       draw_height)
                 : PresentScaleSelection{ScalingFilter::Bilinear, PresentScaleMode::Native};
    const ScalingFilter setting = resolved_scale.filter;

    // The scaling and anti-aliasing chain runs for the base layer only. Its
    // intermediates are single instances sized to whatever last went through
    // them, so two differently shaped layers would reallocate them every frame.
    // Overlay layers and applet captures use a plain bilinear blit.
    FilterChoice choice{Filter::Standard, 0.0f, 0.0f};
    bool already_linear = false;

    if (run_post) {
        // Before scaling and at source resolution, that's where an edge filter
        // still has the geometry it's trying to reconstruct.
        source = AntiAlias(source);
        if (GetAntiAliasing() == AntiAliasing::Smaa) {
            source = SmaaAntiAlias(source);
        }
        choice = ChooseFilter(setting);
    }

    // MetalFx is a scaling filter like any other and not a separate toggle, so
    // there's no precedence rule between it and Fsr. When MetalFX declines (not
    // actually upscaling, a crop it can't express, unsupported hardware) it
    // returns the source and we fall back to a bilinear blit.
    // NxEmu has no MetalFX filter choice yet.
    if (false) {
        MTL::Texture* const scaled = MetalFxUpscale(source, draw_width, draw_height,
                                                    crop_left, crop_top, crop_right, crop_bottom);
        if (scaled != source) {
            source = scaled;
            crop_left = 0.0f;
            crop_top = 0.0f;
            crop_right = 1.0f;
            crop_bottom = 1.0f;
        }
        // Already at display resolution either way. After a successful upscale
        // there's nothing left to resample, and after a declined one there's no
        // MetalFX pass to run.
        choice = ChooseFilter(ScalingFilter::Bilinear);
    }

    // FSR:
    // - FsrEasu renders EASU directly into the swapchain in ONE pass (zero intermediate).
    // - Fsr at 1:1 native resolution (CanUseNativeRcas) skips EASU and runs RCAS directly.
    // - Otherwise EASU runs into a 32-bpp RGB10A2Unorm intermediate and RCAS sharpens it.
    if (run_post && setting == ScalingFilter::FsrEasu) {
        choice = {Filter::FsrEasu, 0.0f, 0.0f};
    } else if (run_post && setting == ScalingFilter::Fsr) {
        const f32 stops =
            static_cast<f32>(videoSettings.fsr_sharpening_slider) / 100.0f;
        const bool native_rcas =
            CanUseNativeRcas(static_cast<u32>(source->width()),
                             static_cast<u32>(source->height()), crop_rect, draw_width,
                             draw_height);
        if (native_rcas) {
            choice = {Filter::FsrRcas, std::exp2(-stops), 0.0f};
        } else {
            MTL::Texture* const upscaled = FsrUpscale(source, draw_width, draw_height, crop_left,
                                                      crop_top, crop_right, crop_bottom);
            if (upscaled != source) {
                source = upscaled;
                already_linear = true;
                crop_left = 0.0f;
                crop_top = 0.0f;
                crop_right = 1.0f;
                crop_bottom = 1.0f;
                choice = {Filter::FsrRcas, std::exp2(-stops), 0.0f};
            } else {
                choice = {Filter::Standard, 0.0f, 0.0f};
            }
        }
    }

    MTL::RenderPipelineState* pipeline =
        GetPipeline(target->pixelFormat(), choice.filter, blending);
    if (pipeline == nullptr && choice.filter != Filter::Standard) {
        // A filter that won't compile must not cost us the frame.
        choice = {Filter::Standard, 0.0f, 0.0f};
        pipeline = GetPipeline(target->pixelFormat(), Filter::Standard, blending);
    }
    if (pipeline == nullptr) {
        return;
    }

    // Linear by default. Nearest when the user asks for it, for the filters that
    // weight texel centres themselves (bilinear on top would filter twice), and
    // for MMPX which compares texels for exact equality. Spline-1 and SGSR have
    // to stay linear, Spline-1 leans on the hardware's interpolation and SGSR
    // wants its centre tap filtered.
    const bool nearest = setting == ScalingFilter::NearestNeighbor ||
                         setting == ScalingFilter::CasNearest ||
                         choice.filter == Filter::Cubic || choice.filter == Filter::Lanczos ||
                         choice.filter == Filter::Gaussian || choice.filter == Filter::Mmpx;
    MTL::SamplerState* sampler =
        nearest ? sampler_nearest.get() : sampler_linear.get();

    // Source texels per output pixel, which is what the area filter averages
    // over. Taken from the cropped region and not the whole texture, so a
    // framebuffer wider than the visible area doesn't inflate the footprint.
    const f32 cropped_width =
        std::abs(crop_right - crop_left) * static_cast<f32>(source->width());
    const f32 cropped_height =
        std::abs(crop_bottom - crop_top) * static_cast<f32>(source->height());
    const std::array<f32, 4> filter_params{
        choice.b,
        choice.c,
        draw_width > 0 ? cropped_width / static_cast<f32>(draw_width) : 1.0f,
        draw_height > 0 ? cropped_height / static_cast<f32>(draw_height) : 1.0f,
    };

    RenderPassState pass{};
    pass.num_color = 1;
    pass.color[0].texture = target;
    const Common::Rectangle<u32> place =
        screen_usable ? screen : Common::Rectangle<u32>{0, 0, target_width, target_height};
    const bool letterboxed = place.GetWidth() != target_width ||
                             place.GetHeight() != target_height || place.left != 0 ||
                             place.top != 0;
    if (is_base_layer) {
        // Cleared whenever the base layer is letterboxed OR blends. DontCare
        // leaves the drawable holding a recycled two-or-three-frame-old image,
        // which shows in the bars and ghosts on a blending base layer. Opaque
        // and full-screen stays DontCare, every pixel is overwritten and
        // skipping the clear skips a tile initialisation.
        const bool blends = blending != Tegra::BlendMode::Opaque;
        pass.color[0].load_action =
            (letterboxed || blends) ? LoadAction::Clear : LoadAction::DontCare;
        pass.color[0].clear_color = {
            static_cast<f64>(videoSettings.bg_red) / 255.0,
            static_cast<f64>(videoSettings.bg_green) / 255.0,
            static_cast<f64>(videoSettings.bg_blue) / 255.0, 1.0};
    } else {
        pass.color[0].load_action = LoadAction::Load;
    }
    pass.color[0].store_action = StoreAction::Store;
    pass.render_area_width = target_width;
    pass.render_area_height = target_height;

    const std::array<f32, 4> crop{crop_left, crop_top, crop_right, crop_bottom};
    // Whether the shader decodes the sample itself. A non-sRGB source hands back
    // bits that are already sRGB-encoded, so they need decoding before the sRGB
    // drawable re-encodes them. After EASU the intermediate is already linear
    // float, so RCAS must not decode again or the gamma goes on twice.
    const u32 source_is_linear =
        already_linear ? 0u : (IsSrgbFormat(source->pixelFormat()) ? 0u : 1u);
    // Opened inside the recorded command and not before it. RequestRenderPass
    // and Record each take and release chunk_mutex separately, so anything
    // recording in between can end the encoder and drop this draw. Requesting on
    // the worker makes opening and drawing one step, at the cost of telling the
    // recording-side mirror by hand.
    scheduler.NotifyRenderPassOpened(pass);
    scheduler.Record([=](CommandContext& context) {
        RenderEncoder encoder = context.RequestRender(pass);
        if (encoder == nullptr) {
            static std::atomic<u32> reported{0};
            if (reported++ < 4) {
                LOG_ERROR(Render_Metal, "Present draw dropped: no render encoder");
            }
            return;
        }

        MTL::Viewport viewport{};
        viewport.originX = static_cast<double>(place.left);
        viewport.originY = static_cast<double>(place.top);
        viewport.width = static_cast<double>(place.GetWidth());
        viewport.height = static_cast<double>(place.GetHeight());
        viewport.znear = 0.0;
        viewport.zfar = 1.0;
        encoder->setViewport(viewport);

        encoder->setRenderPipelineState(pipeline);
        encoder->setCullMode(MTL::CullModeNone);
        encoder->setVertexBytes(crop.data(), crop.size() * sizeof(f32), CROP_BUFFER_INDEX);
        encoder->setFragmentTexture(source, SOURCE_TEXTURE_INDEX);
        encoder->setFragmentSamplerState(sampler, SOURCE_SAMPLER_INDEX);
        encoder->setFragmentBytes(&source_is_linear, sizeof(source_is_linear),
                                  SOURCE_IS_LINEAR_INDEX);
        encoder->setFragmentBytes(filter_params.data(), filter_params.size() * sizeof(f32),
                                  FILTER_PARAMS_INDEX);

        encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger{0}, NS::UInteger{3});
    });
}

} // namespace Metal
