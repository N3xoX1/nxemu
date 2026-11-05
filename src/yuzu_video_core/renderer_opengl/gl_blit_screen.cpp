// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/settings.h"
#include "frontend/framebuffer_layout.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/renderer_opengl/gl_blit_screen.h"
#include "yuzu_video_core/renderer_opengl/gl_state_tracker.h"
#include "yuzu_video_core/renderer_opengl/present/filters.h"
#include "yuzu_video_core/renderer_opengl/present/diagnostics.h"
#include "yuzu_video_core/renderer_opengl/present/layer.h"
#include "yuzu_video_core/renderer_opengl/present/window_adapt_pass.h"

namespace OpenGL {

BlitScreen::BlitScreen(RasterizerOpenGL& rasterizer_,
                       Tegra::MaxwellDeviceMemoryManager& device_memory_,
                       StateTracker& state_tracker_, ProgramManager& program_manager_,
                       Device& device_, const PresentFilters& filters_)
    : rasterizer(rasterizer_), device_memory(device_memory_), state_tracker(state_tracker_),
      program_manager(program_manager_), device(device_), filters(filters_) {}

BlitScreen::~BlitScreen() = default;

void BlitScreen::DrawScreen(std::span<const Tegra::FramebufferConfig> framebuffers,
                            const Layout::FramebufferLayout& layout, bool invert_y) {
    VideoCore::BeginFsrFrame(videoSettings.fsr_sharpening_slider, videoSettings.cas_sharpening_slider,
                            static_cast<unsigned>(videoSettings.scaling_filter), GetDownscalingFilter());
    // TODO: Signal state tracker about these changes
    state_tracker.NotifyScreenDrawVertexArray();
    state_tracker.NotifyPolygonModes();
    state_tracker.NotifyViewport0();
    state_tracker.NotifyScissor0();
    state_tracker.NotifyColorMask(0);
    state_tracker.NotifyBlend0();
    state_tracker.NotifyFramebuffer();
    state_tracker.NotifyFrontFace();
    state_tracker.NotifyCullTest();
    state_tracker.NotifyDepthTest();
    state_tracker.NotifyStencilTest();
    state_tracker.NotifyPolygonOffset();
    state_tracker.NotifyRasterizeEnable();
    state_tracker.NotifyFramebufferSRGB();
    state_tracker.NotifyLogicOp();
    state_tracker.NotifyClipControl();
    state_tracker.NotifyAlphaTest();
    state_tracker.ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);

    glEnable(GL_CULL_FACE);
    glDisable(GL_COLOR_LOGIC_OP);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_ALPHA_TEST);
    glDisablei(GL_BLEND, 0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthRangeIndexed(0, 0.0, 0.0);

    CreateWindowAdapt();
    while (layers.size() < framebuffers.size()) {
        layers.emplace_back(rasterizer, device_memory, filters);
    }

    const bool measure = VideoCore::PresentDiagnostics::Enabled();
    VideoCore::PresentDiagnostics::Clock::time_point start;
    if (measure) {
        if (!timer) timer = std::make_unique<PresentTimer>();
        timer->Begin();
        start = VideoCore::PresentDiagnostics::Clock::now();
    }
    window_adapt->DrawToFramebuffer(program_manager, layers, framebuffers, layout, invert_y, current_window_adapt);
    VideoCore::PublishPresentationStatus(filters.is_applet_capture, layout.screen.GetWidth(), layout.screen.GetHeight(),
                                        static_cast<unsigned>(framebuffers.size()));
    if (measure) {
        auto sample = VideoCore::PresentDiagnostics::Begin(layout.screen.GetWidth(), layout.screen.GetHeight(),
            static_cast<unsigned>(framebuffers.size()), static_cast<unsigned>(current_window_adapt));
        sample.is_applet = filters.is_applet_capture;
        sample.cpu_ms = std::chrono::duration<double, std::milli>(VideoCore::PresentDiagnostics::Clock::now() - start).count();
        timer->End(sample);
    }

    // TODO
    // program_manager.RestoreGuestPipeline();
}

void BlitScreen::CreateWindowAdapt() {
    if (window_adapt && filters.get_scaling_filter() == current_window_adapt && legacy_fsr == VideoCore::LegacyFsr()) {
        return;
    }

    current_window_adapt = filters.get_scaling_filter();
    legacy_fsr = VideoCore::LegacyFsr();
    layers.clear();
    switch (current_window_adapt) {
    case ScalingFilter::CatmullRom:
    case ScalingFilter::CatmullRomFast:
        window_adapt = MakeCatmullRom(device);
        break;
    case ScalingFilter::CasNearest:
    case ScalingFilter::CasBilinear:
    case ScalingFilter::CasCatmullRom:
    case ScalingFilter::CasCatmullRomFast:
        window_adapt = MakeCas(device, current_window_adapt);
        break;
    case ScalingFilter::NearestNeighbor:
        window_adapt = MakeNearestNeighbor(device);
        break;
    case ScalingFilter::Bicubic:
        window_adapt = MakeBicubic(device);
        break;
    case ScalingFilter::Gaussian:
        window_adapt = MakeGaussian(device);
        break;
    case ScalingFilter::ScaleForceFast:
        window_adapt = MakeScaleForceFast(device);
        break;
    case ScalingFilter::ScaleForce:
        window_adapt = MakeScaleForce(device);
        break;
    case ScalingFilter::Fsr:
        window_adapt = VideoCore::LegacyFsr() ? MakeBilinear(device) : MakeFsrRcas(device);
        break;
    case ScalingFilter::FsrEasu:
        window_adapt = MakeFsrEasu(device);
        break;
    case ScalingFilter::Bilinear:
    default:
        window_adapt = MakeBilinear(device);
        break;
    }
}

} // namespace OpenGL
