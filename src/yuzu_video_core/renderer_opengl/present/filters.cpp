// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/host_shaders/opengl_present_frag.h"
#include "yuzu_video_core/host_shaders/catmull_rom_glsl.h"
#include "yuzu_video_core/host_shaders/present_catmull_rom_frag.h"
#include "yuzu_video_core/host_shaders/opengl_present_cas_frag.h"
#include "yuzu_video_core/host_shaders/opengl_fidelityfx_fsr_frag.h"
#include "yuzu_video_core/host_shaders/ffx_a_h.h"
#include "yuzu_video_core/host_shaders/ffx_fsr1_h.h"
#include "yuzu_video_core/host_shaders/opengl_present_scaleforce_frag.h"
#include "yuzu_video_core/host_shaders/present_area_frag.h"
#include "yuzu_video_core/host_shaders/present_bicubic_frag.h"
#include "yuzu_video_core/host_shaders/present_gaussian_frag.h"
#include "yuzu_video_core/renderer_opengl/present/filters.h"
#include "yuzu_video_core/renderer_opengl/present/util.h"

namespace OpenGL {

std::unique_ptr<WindowAdaptPass> MakeCatmullRom(const Device& device) {
    std::string source{HostShaders::PRESENT_CATMULL_ROM_FRAG};
    ReplaceInclude(source, "catmull_rom.glsl", HostShaders::CATMULL_ROM_GLSL);
    return std::make_unique<WindowAdaptPass>(device, CreateBilinearSampler(), source, false, false, true);
}

std::unique_ptr<WindowAdaptPass> MakeCas(const Device& device, ScalingFilter filter) {
    const bool catmull = filter == ScalingFilter::CasCatmullRom || filter == ScalingFilter::CasCatmullRomFast;
    std::string source = fmt::format("#version 460 core\n{}\n{}", catmull ? "#define CAS_CATMULL 1" : "",
                                   HostShaders::OPENGL_PRESENT_CAS_FRAG);
    ReplaceInclude(source, "catmull_rom.glsl", HostShaders::CATMULL_ROM_GLSL);
    return std::make_unique<WindowAdaptPass>(device,
        filter == ScalingFilter::CasNearest ? CreateNearestNeighborSampler() : CreateBilinearSampler(),
        source, false, false, catmull, true);
}

std::unique_ptr<WindowAdaptPass> MakeNearestNeighbor(const Device& device) {
    return std::make_unique<WindowAdaptPass>(device, CreateNearestNeighborSampler(),
                                             HostShaders::OPENGL_PRESENT_FRAG);
}

std::unique_ptr<WindowAdaptPass> MakeArea(const Device& device) {
    return std::make_unique<WindowAdaptPass>(device, CreateBilinearSampler(),
        HostShaders::PRESENT_AREA_FRAG);
}

std::unique_ptr<WindowAdaptPass> MakeBilinear(const Device& device) {
    return std::make_unique<WindowAdaptPass>(device, CreateBilinearSampler(),
                                             HostShaders::OPENGL_PRESENT_FRAG);
}

std::unique_ptr<WindowAdaptPass> MakeFsrEasu(const Device& device) {
    std::string shader_source =
        fmt::format("#version 460\n#define USE_EASU 1\n#define YUZU_PRESENT_EASU 1\n{}",
                    HostShaders::OPENGL_FIDELITYFX_FSR_FRAG);
    ReplaceInclude(shader_source, "ffx_a.h", HostShaders::FFX_A_H);
    ReplaceInclude(shader_source, "ffx_fsr1.h", HostShaders::FFX_FSR1_H);
    return std::make_unique<WindowAdaptPass>(device, CreateBilinearSampler(), shader_source,
                                             false, true);
}

std::unique_ptr<WindowAdaptPass> MakeFsrRcas(const Device& device) {
    std::string shader_source =
        fmt::format("#version 460\n#define USE_RCAS 1\n#define frag_texcoord frag_tex_coord\n{}",
                    HostShaders::OPENGL_FIDELITYFX_FSR_FRAG);
    ReplaceInclude(shader_source, "ffx_a.h", HostShaders::FFX_A_H);
    ReplaceInclude(shader_source, "ffx_fsr1.h", HostShaders::FFX_FSR1_H);
    return std::make_unique<WindowAdaptPass>(device, CreateBilinearSampler(), shader_source, true);
}

std::unique_ptr<WindowAdaptPass> MakeBicubic(const Device& device) {
    return std::make_unique<WindowAdaptPass>(device, CreateBilinearSampler(),
                                             HostShaders::PRESENT_BICUBIC_FRAG);
}

std::unique_ptr<WindowAdaptPass> MakeGaussian(const Device& device) {
    return std::make_unique<WindowAdaptPass>(device, CreateBilinearSampler(),
                                             HostShaders::PRESENT_GAUSSIAN_FRAG);
}

std::unique_ptr<WindowAdaptPass> MakeScaleForce(const Device& device) {
    return std::make_unique<WindowAdaptPass>(
        device, CreateBilinearSampler(),
        fmt::format("#version 460\n{}", HostShaders::OPENGL_PRESENT_SCALEFORCE_FRAG));
}

std::unique_ptr<WindowAdaptPass> MakeScaleForceFast(const Device& device) {
    return std::make_unique<WindowAdaptPass>(device, CreateBilinearSampler(),
        fmt::format("#version 460\n#define SCALEFORCE_FAST 1\n{}", HostShaders::OPENGL_PRESENT_SCALEFORCE_FRAG));
}

} // namespace OpenGL
