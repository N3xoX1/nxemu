// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <variant>

#include "yuzu_common/dynamic_library.h"
#include "yuzu_video_core/host1x/gpu_device_memory_manager.h"
#include "yuzu_video_core/renderer_base.h"
#include "yuzu_video_core/renderer_vulkan/vk_blit_screen.h"
#include "yuzu_video_core/renderer_vulkan/vk_present_manager.h"
#include "yuzu_video_core/renderer_vulkan/vk_rasterizer.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_state_tracker.h"
#include "yuzu_video_core/renderer_vulkan/vk_swapchain.h"
#include "yuzu_video_core/renderer_vulkan/vk_turbo_mode.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Memory {
class Memory;
}

namespace Tegra {
class GPU;
}

namespace Vulkan {

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld,
                    VkSurfaceKHR surface);

class RendererVulkan final : public VideoCore::RendererBase {
    struct watermark_vk
    {
        vk::Image image;
        vk::ImageView image_view;
        vk::Sampler sampler;
        vk::DescriptorSetLayout dsl;
        vk::DescriptorPool dpool;
        VkDescriptorSet set;
        vk::RenderPass render_pass;
        vk::Pipeline pipeline;
        vk::PipelineLayout pipeline_layout;
        vk::Buffer vertex_buffer;

        std::chrono::steady_clock::time_point start_time;
        bool timer_started;

        int width, height;
        bool valid;
        bool show;
    };

public:
    explicit RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                            Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                            std::unique_ptr<Core::Frontend::GraphicsContext> context_);
    ~RendererVulkan() override;

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffers) override;

    std::vector<u8> GetAppletCaptureBuffer() override;

    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return device.GetDriverName();
    }

private:
    void InitializeWatermark();
    void CleanupWatermark();
    void RenderWatermark(Frame & frame);
    void CreateWatermarkRenderPass();
    void CreateWatermarkPipeline();
    void CreateWatermarkVertexBuffer();
    void Report() const;

    vk::Buffer RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                              const Layout::FramebufferLayout& layout, VkFormat format,
                              VkDeviceSize buffer_size);
    void RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers);
    void RenderAppletCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers);

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;
    watermark_vk wm;
    std::mutex composite_mutex;

    std::shared_ptr<Common::DynamicLibrary> library;
    vk::InstanceDispatch dld;

    vk::Instance instance;
    vk::DebugUtilsMessenger debug_messenger;
    vk::SurfaceKHR surface;

    Device device;
    MemoryAllocator memory_allocator;
    StateTracker state_tracker;
    Scheduler scheduler;
    Swapchain swapchain;
    PresentManager present_manager;
    BlitScreen blit_swapchain;
    BlitScreen blit_capture;
    BlitScreen blit_applet;
    RasterizerVulkan rasterizer;
    std::optional<TurboMode> turbo_mode;

    Frame applet_frame;
};

} // namespace Vulkan
