// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <stdexcept>

#include "frontend/graphics_context.h"
#include "video_settings.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "yuzu_video_core/host1x/gpu_device_memory_manager.h"
#include "yuzu_video_core/host1x/host1x.h"
#include "yuzu_video_core/renderer_base.h"
#include "yuzu_video_core/renderer_null/renderer_null.h"
#include "yuzu_video_core/renderer_opengl/renderer_opengl.h"
#include "yuzu_video_core/renderer_vulkan/renderer_vulkan.h"
#include "yuzu_video_core/renderer_vulkan/vk_display.h"
#include "yuzu_video_core/video_core.h"

namespace {

std::unique_ptr<VideoCore::RendererBase> CreateRenderer(Tegra::Host1x::Host1x & host1x, Core::Frontend::EmuWindow & emu_window, Tegra::GPU & gpu, std::unique_ptr<Core::Frontend::GraphicsContext> context, Vulkan::Display * display)
{
    auto & device_memory = host1x.MemoryManager();

    switch (videoSettings.renderer_backend)
    {
    case RendererBackend::OpenGL: return std::make_unique<OpenGL::RendererOpenGL>(emu_window, device_memory, gpu, std::move(context));
    case RendererBackend::Vulkan:
        if (display == nullptr)
        {
            throw std::runtime_error{"No Vulkan display"};
        }
        return std::make_unique<Vulkan::RendererVulkan>(emu_window, device_memory, gpu, std::move(context), *display);
    case RendererBackend::Null: return std::make_unique<Null::RendererNull>(emu_window, gpu, std::move(context));
    }
    return nullptr;
}

} // Anonymous namespace

namespace VideoCore {

std::unique_ptr<Tegra::GPU> CreateGPU(ISystemModules & modules, Core::Frontend::EmuWindow& emu_window, Tegra::Host1x::Host1x & host1x, Vulkan::Display * display)
{
    Settings::UpdateRescalingInfo();

    const auto nvdec_value = videoSettings.nvdec_emulation;
    const bool use_nvdec = nvdec_value != NvdecEmulation::Off;
    const bool use_async = videoSettings.use_asynchronous_gpu_emulation;
    auto gpu = std::make_unique<Tegra::GPU>(modules, host1x, use_async, use_nvdec);
    auto context = emu_window.CreateSharedContext();
    auto scope = context->Acquire();
    try {
        auto renderer = CreateRenderer(host1x, emu_window, *gpu, std::move(context), display);
        gpu->BindRenderer(std::move(renderer));
        return gpu;
    } catch (const std::runtime_error& exception) {
        scope.Cancel();
        LOG_ERROR(HW_GPU, "Failed to initialize GPU: {}", exception.what());
        return nullptr;
    }
}

void DisplayDeleter::operator()(Vulkan::Display * display) const
{
    delete display;
}

DisplayPtr CreateDisplay(Core::Frontend::EmuWindow & emu_window)
{
    try {
        const auto context = emu_window.CreateSharedContext();
        const auto & layout = emu_window.GetFramebufferLayout();
        return DisplayPtr{new Vulkan::Display(emu_window.GetWindowInfo(), context.get(), layout.width, layout.height)};
    } catch (const std::exception & exception) {
        LOG_ERROR(Render_Vulkan, "Vulkan initialization failed with error: {}", exception.what());
        return nullptr;
    }
}

bool AttachInterface(Vulkan::Display & display, VideoInterfaceDevice & device)
{
    try {
        display.AttachInterface();
    } catch (const std::exception & exception) {
        LOG_ERROR(Render_Vulkan, "Could not set up the interface: {}", exception.what());
        return false;
    }
    device.getInstanceProcAddr = reinterpret_cast<void *>(display.GetDispatch().vkGetInstanceProcAddr);
    device.instance = *display.GetInstance();
    device.physicalDevice = static_cast<VkPhysicalDevice>(display.GetDevice().GetPhysical());
    device.device = *display.GetDevice().GetLogical();
    return true;
}

bool BeginInterfaceFrame(Vulkan::Display & display, uint32_t width, uint32_t height, VideoInterfaceFrame & frame)
{
    try {
        VkImage image{};
        frame.commandBuffer = display.BeginInterfaceFrame(width, height, image);
        frame.image = reinterpret_cast<uint64_t>(image);
        return true;
    } catch (const std::exception & exception) {
        LOG_ERROR(Render_Vulkan, "Could not start a frame of the interface: {}", exception.what());
        return false;
    }
}

void EndInterfaceFrame(Vulkan::Display & display, const VideoInterfaceArea * game_area)
{
    try {
        if (game_area == nullptr) {
            display.EndInterfaceFrame(nullptr);
            return;
        }
        const Vulkan::DisplayArea area{game_area->x, game_area->y, game_area->width, game_area->height};
        display.EndInterfaceFrame(&area);
    } catch (const std::exception & exception) {
        LOG_ERROR(Render_Vulkan, "Could not show a frame of the interface: {}", exception.what());
    }
}

void WaitInterfaceIdle(Vulkan::Display & display)
{
    display.WaitInterfaceIdle();
}

} // namespace VideoCore
