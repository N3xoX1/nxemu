// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <list>
#include "nxemu-module-spec/video.h"
#include <span>

#include "yuzu_common/math_util.h"
#include "yuzu_video_core/renderer_opengl/gl_resource_manager.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Tegra {
struct FramebufferConfig;
}

namespace OpenGL {

class Device;
class Layer;
class ProgramManager;

class WindowAdaptPass final {
public:
    explicit WindowAdaptPass(const Device& device, OGLSampler&& sampler,
                             std::string_view frag_source, bool use_rcas = false, bool use_easu = false,
                             bool use_catmull = false, bool use_cas = false);
    ~WindowAdaptPass();

    void DrawToFramebuffer(ProgramManager& program_manager, std::list<Layer>& layers,
                           std::span<const Tegra::FramebufferConfig> framebuffers,
                           const Layout::FramebufferLayout& layout, bool invert_y, ScalingFilter requested_filter);

private:
    const Device& device;
    const bool use_rcas;
    const bool use_easu;
    const bool use_catmull;
    const bool use_cas;
    OGLSampler sampler;
    OGLProgram vert;
    OGLProgram frag;
    OGLProgram bilinear_frag, area_frag;
    OGLSampler fallback_sampler;
    OGLBuffer vertex_buffer;

    // GPU address of the vertex buffer
    GLuint64EXT vertex_buffer_address = 0;
};

} // namespace OpenGL
