// SPDX-License-Identifier: GPL-2.0-or-later
#version 460 core
layout(location=0) in vec2 frag_tex_coord;
layout(location=0) out vec4 color;
layout(binding=0) uniform sampler2D color_texture;
layout(push_constant) uniform CaptureConstants { layout(offset=80) uint dimensions_only; };
void main() {
    if (dimensions_only != 0u) {
        uvec2 size = uvec2(textureSize(color_texture,0));
        color = vec4(size.x & 255u, size.x >> 8u, size.y & 255u, size.y >> 8u) / 255.0;
    } else {
        color = textureLod(color_texture,frag_tex_coord,0.0);
    }
}
