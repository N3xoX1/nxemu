// SPDX-License-Identifier: GPL-2.0-or-later
#version 460 core
layout(location = 0) in vec2 frag_tex_coord;
layout(location = 0) out vec4 color;
layout(binding = 0) uniform sampler2D color_texture;

// Independent, direct 4x4 evaluation. No hardware bilinear grouping.
float Cubic(float x) {
    x = abs(x);
    if (x < 1.0) return ((1.5 * x - 2.5) * x) * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}
void main() {
    ivec2 size = textureSize(color_texture, 0);
    vec2 p = frag_tex_coord * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(p));
    vec2 f = fract(p);
    vec4 value = vec4(0.0);
    for (int y = -1; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            value += texelFetch(color_texture, clamp(base + ivec2(x, y), ivec2(0), size - 1), 0)
                   * Cubic(float(x) - f.x) * Cubic(float(y) - f.y);
        }
    }
    color = clamp(value, 0.0, 1.0);
}
