// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>

#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "yuzu_video_core/host1x/gpu_device_memory_manager.h"
#include "yuzu_video_core/host1x/host1x.h"
#include "yuzu_video_core/renderer_base.h"
#include "yuzu_video_core/renderer_null/renderer_null.h"
#include "yuzu_video_core/renderer_opengl/renderer_opengl.h"
#include "yuzu_video_core/renderer_vulkan/renderer_vulkan.h"
#include "yuzu_video_core/video_core.h"
#include "video_settings.h"

namespace {

std::unique_ptr<VideoCore::RendererBase> CreateRenderer(Tegra::Host1x::Host1x & host1x, Core::Frontend::EmuWindow & emu_window, Tegra::GPU & gpu, std::unique_ptr<Core::Frontend::GraphicsContext> context)
{
    auto & device_memory = host1x.MemoryManager();

    switch (videoSettings.renderer_backend.GetValue())
    {
    case RendererBackend::OpenGL: return std::make_unique<OpenGL::RendererOpenGL>(emu_window, device_memory, gpu, std::move(context));
    case RendererBackend::Vulkan: return std::make_unique<Vulkan::RendererVulkan>(emu_window, device_memory, gpu, std::move(context));
    case RendererBackend::Null: return std::make_unique<Null::RendererNull>(emu_window, gpu, std::move(context));
    }
    return nullptr;
}

} // Anonymous namespace

namespace VideoCore {

std::unique_ptr<Tegra::GPU> CreateGPU(ISystemModules & modules, Core::Frontend::EmuWindow& emu_window, Tegra::Host1x::Host1x & host1x)
{
    Settings::UpdateRescalingInfo();

    const auto nvdec_value = videoSettings.nvdec_emulation.GetValue();
    const bool use_nvdec = nvdec_value != NvdecEmulation::Off;
    const bool use_async = videoSettings.use_asynchronous_gpu_emulation.GetValue();
    auto gpu = std::make_unique<Tegra::GPU>(modules, host1x, use_async, use_nvdec);
    auto context = emu_window.CreateSharedContext();
    auto scope = context->Acquire();
    try {
        auto renderer = CreateRenderer(host1x, emu_window, *gpu, std::move(context));
        gpu->BindRenderer(std::move(renderer));
        return gpu;
    } catch (const std::runtime_error& exception) {
        scope.Cancel();
        LOG_ERROR(HW_GPU, "Failed to initialize GPU: {}", exception.what());
        return nullptr;
    }
}

} // namespace VideoCore
