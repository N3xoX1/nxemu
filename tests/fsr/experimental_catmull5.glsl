// SPDX-License-Identifier: GPL-2.0-or-later
// EXPERIMENT ONLY: normalized five-tap approximation of Catmull-Rom.
// Omits four corner products, so this is not the exact nine-tap filter.
// Kept outside production until its visual tradeoff is evaluated in game.
vec4 CatmullRomWeights(float t) {
    return vec4(t * (-0.5 + t * (1.0 - 0.5 * t)),
                1.0 + t * t * (-2.5 + 1.5 * t),
                t * (0.5 + t * (2.0 - 1.5 * t)),
                t * t * (-0.5 + 0.5 * t));
}

vec4 SampleCatmullRom(sampler2D source, vec2 uv, vec2 size, vec2 inv_size) {
    vec2 position = uv * size - 0.5;
    vec2 base = floor(position);
    vec2 fraction = position - base;
    vec4 wx = CatmullRomWeights(fraction.x);
    vec4 wy = CatmullRomWeights(fraction.y);
    vec3 weights_x = vec3(wx.x, wx.y + wx.z, wx.w);
    vec3 weights_y = vec3(wy.x, wy.y + wy.z, wy.w);
    // The middle weight is >= 1 throughout [0, 1], so division is safe.
    vec3 x = (base.x + vec3(-0.5, 0.5 + wx.z / weights_x.y, 2.5)) * inv_size.x;
    vec3 y = (base.y + vec3(-0.5, 0.5 + wy.z / weights_y.y, 2.5)) * inv_size.y;
    // Experimental five-tap approximation: omit the four corner products,
    // then normalize the surviving kernel to preserve constant colors.
    vec4 cross_sum = textureLod(source, vec2(x.y, y.x), 0.0) * weights_x.y * weights_y.x
                   + textureLod(source, vec2(x.x, y.y), 0.0) * weights_x.x * weights_y.y
                   + textureLod(source, vec2(x.y, y.y), 0.0) * weights_x.y * weights_y.y
                   + textureLod(source, vec2(x.z, y.y), 0.0) * weights_x.z * weights_y.y
                   + textureLod(source, vec2(x.y, y.z), 0.0) * weights_x.y * weights_y.z;
    float normalization = weights_x.y + weights_y.y - weights_x.y * weights_y.y;
    return clamp(cross_sum / normalization, 0.0, 1.0);
}
