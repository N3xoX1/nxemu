// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_video_core/fsr.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/host_shaders/ffx_a_h.h"
#include "yuzu_video_core/host_shaders/ffx_fsr1_h.h"
#include "yuzu_video_core/host_shaders/full_screen_triangle_vert.h"
#include "yuzu_video_core/host_shaders/opengl_fidelityfx_fsr_easu_frag.h"
#include "yuzu_video_core/host_shaders/opengl_fidelityfx_fsr_frag.h"
#include "yuzu_video_core/host_shaders/opengl_fidelityfx_fsr_rcas_frag.h"
#include "yuzu_video_core/renderer_opengl/gl_shader_manager.h"
#include "yuzu_video_core/renderer_opengl/gl_shader_util.h"
#include "yuzu_video_core/renderer_opengl/present/fsr.h"
#include "yuzu_video_core/renderer_opengl/present/util.h"

namespace OpenGL {
using namespace FSR;

using FsrConstants = std::array<u32, 4 * 4>;

FSR::FSR(u32 output_width_, u32 output_height_, bool easu_only)
    : width(output_width_), height(output_height_), m_easu_only(easu_only) {
    std::string fsr_source{HostShaders::OPENGL_FIDELITYFX_FSR_FRAG};
    ReplaceInclude(fsr_source, "ffx_a.h", HostShaders::FFX_A_H);
    ReplaceInclude(fsr_source, "ffx_fsr1.h", HostShaders::FFX_FSR1_H);

    std::string fsr_easu_source{HostShaders::OPENGL_FIDELITYFX_FSR_EASU_FRAG};
    std::string fsr_rcas_source{HostShaders::OPENGL_FIDELITYFX_FSR_RCAS_FRAG};
    ReplaceInclude(fsr_easu_source, "opengl_fidelityfx_fsr.frag", fsr_source);
    ReplaceInclude(fsr_rcas_source, "opengl_fidelityfx_fsr.frag", fsr_source);

    vert = CreateProgram(HostShaders::FULL_SCREEN_TRIANGLE_VERT, GL_VERTEX_SHADER);
    easu_frag = CreateProgram(fsr_easu_source, GL_FRAGMENT_SHADER);
    if (!m_easu_only) {
        rcas_frag = CreateProgram(fsr_rcas_source, GL_FRAGMENT_SHADER);
    }

    glProgramUniform2f(vert.handle, 0, 1.0f, -1.0f);
    glProgramUniform2f(vert.handle, 1, 0.0f, 1.0f);

    sampler = CreateBilinearSampler();
    framebuffer.Create();

    easu_tex.Create(GL_TEXTURE_2D);
    glTextureStorage2D(easu_tex.handle, 1, GL_RGBA16F, width, height);

    if (!m_easu_only) {
        rcas_tex.Create(GL_TEXTURE_2D);
        glTextureStorage2D(rcas_tex.handle, 1, GL_RGBA16F, width, height);
    }
}

FSR::~FSR() = default;

GLuint FSR::Draw(ProgramManager& program_manager, GLuint texture, u32 input_image_width,
                 u32 input_image_height, const Common::Rectangle<f32>& crop_rect) {
    const f32 input_width = static_cast<f32>(input_image_width);
    const f32 input_height = static_cast<f32>(input_image_height);
    const f32 output_width = static_cast<f32>(width);
    const f32 output_height = static_cast<f32>(height);
    const f32 viewport_width = (crop_rect.right - crop_rect.left) * input_width;
    const f32 viewport_x = crop_rect.left * input_width;
    const f32 viewport_height = (crop_rect.bottom - crop_rect.top) * input_height;
    const f32 viewport_y = crop_rect.top * input_height;

    FsrConstants easu_con{};
    FsrConstants rcas_con{};

    const bool native_rcas = !VideoCore::LegacyFsr() && CanUseNativeRcas(input_image_width, input_image_height, crop_rect,
                                              width, height);
    if (m_easu_only && native_rcas) {
        return texture;
    }
    if (!native_rcas) {
        FsrEasuConOffset(easu_con.data() + 0, easu_con.data() + 4, easu_con.data() + 8,
                         easu_con.data() + 12, viewport_width, viewport_height, input_width,
                         input_height, output_width, output_height, viewport_x, viewport_y);
    }

    if (!m_easu_only) {
        const auto strength = SharpeningStrength::FromLegacyRcas(
            videoSettings.scaling_filter == ScalingFilter::FsrEasu
                ? FsrSharpness::StoredMaximum : videoSettings.fsr_sharpening_slider);
        FsrRcasConFromStrength(rcas_con.data(), strength);
    }
    if (!native_rcas) {
        glProgramUniform4uiv(easu_frag.handle, 0, 4, easu_con.data());
    }
    if (!m_easu_only) {
        glProgramUniform4uiv(rcas_frag.handle, 0, 1, rcas_con.data());
    }

    glFrontFace(GL_CW);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer.handle);
    glViewportIndexedf(0, 0.0f, 0.0f, output_width, output_height);
    glBindSampler(0, sampler.handle);
    if (!native_rcas) {
        glNamedFramebufferTexture(framebuffer.handle, GL_COLOR_ATTACHMENT0, easu_tex.handle, 0);
        program_manager.BindPresentPrograms(vert.handle, easu_frag.handle);
        glBindTextureUnit(0, texture);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    if (m_easu_only) {
        return easu_tex.handle;
    }
    glNamedFramebufferTexture(framebuffer.handle, GL_COLOR_ATTACHMENT0, rcas_tex.handle, 0);
    program_manager.BindPresentPrograms(vert.handle, rcas_frag.handle);
    glBindTextureUnit(0, native_rcas ? texture : easu_tex.handle);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    return rcas_tex.handle;
}

bool FSR::NeedsRecreation(const Common::Rectangle<u32>& screen, bool easu_only) const {
    return screen.GetWidth() != width || screen.GetHeight() != height ||
           m_easu_only != easu_only;
}

} // namespace OpenGL
