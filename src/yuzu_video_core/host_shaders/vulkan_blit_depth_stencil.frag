// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout(binding = 0) uniform sampler2D depth_tex;

layout(location = 0) in vec2 texcoord;

void main() {
    gl_FragDepth = textureLod(depth_tex, texcoord, 0).r;
}
