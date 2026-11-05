// SPDX-FileCopyrightText: Copyright 2026 NXEmu contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Reimplements Intel DeferredColorApply2x2CS's one-sample resolve as a
// fullscreen fragment pass: original source pixels are copied *implicitly*
// into the output framebuffer, avoiding the RGBA16F compute pre-copy.
// Linked-list weights and the R11G11B10_FLOAT HDR packing match Intel CMAA2.
#version 450
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 8) uniform texture2D source_texture;
layout(set = 0, binding = 12) uniform sampler point_sampler;
layout(set = 0, binding = 5, r32ui) readonly uniform uimage2D list_heads;
layout(set = 0, binding = 4, std430) readonly buffer BlendItems {
    uvec2 items[];
} blend_items;

vec3 unpack_intel_hdr(uint value) {
    // Intel CMAA2.hlsl: Unpack_R11G11B10_FLOAT (the HDR mode used in NXEmu).
    float red   = unpackHalf2x16((value << 4u) & 0x7FF0u).x;
    float green = unpackHalf2x16((value >> 7u) & 0x7FF0u).x;
    float blue  = unpackHalf2x16((value >> 17u) & 0x7FE0u).x;
    return vec3(red, green, blue);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 source = texelFetch(sampler2D(source_texture, point_sampler), pixel, 0);
    uint header = imageLoad(list_heads, pixel / 2).r;
    uint quad_offset = uint((pixel.y & 1) * 2 + (pixel.x & 1));
    vec4 total = vec4(0.0);
    // Original Intel loop bound for one-sample CMAA2; hostile/stale links
    // cannot loop indefinitely. Use buffer length to reject invalid indices.
    for (int i = 0; i < 32 && header != 0xFFFFFFFFu; ++i) {
        uint index = header & ((1u << 26u) - 1u);
        if (index >= uint(blend_items.items.length())) break;
        uvec2 item = blend_items.items[index];
        uint next_header = item.x;
        if (((header >> 30u) & 3u) == quad_offset) {
            float weight = 0.8 + float((header >> 26u) & 1u);
            total += vec4(unpack_intel_hdr(item.y) * weight, weight);
        }
        header = next_header;
    }
    out_color = total.a > 0.0 ? vec4(total.rgb / total.a, source.a) : source;
}
