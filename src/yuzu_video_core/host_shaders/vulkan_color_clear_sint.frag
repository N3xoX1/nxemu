// SPDX-License-Identifier: GPL-2.0-or-later

#version 460 core

layout(push_constant) uniform PushConstants {
    ivec4 clear_color;
};

layout(location = 0) out ivec4 color;

void main() {
    color = clear_color;
}
