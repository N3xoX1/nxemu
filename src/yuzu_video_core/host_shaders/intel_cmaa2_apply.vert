// SPDX-FileCopyrightText: Copyright 2026 NXEmu contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#version 450
// Full-screen triangle, one invocation per output pixel after rasterization.
void main() {
    float x = float((gl_VertexIndex & 1) << 2);
    float y = float((gl_VertexIndex & 2) << 1);
    gl_Position = vec4(x - 1.0, y - 1.0, 0.0, 1.0);
}
