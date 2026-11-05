// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/framebuffer_config.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/present_sharpening.h"
#include "yuzu_video_core/renderer_vulkan/present/filters.h"
#include "yuzu_video_core/renderer_vulkan/present/scale_capture.h"
#include "yuzu_video_core/renderer_vulkan/present/diagnostics.h"
#include "yuzu_video_core/renderer_vulkan/present/layer.h"
#include "yuzu_video_core/renderer_vulkan/vk_blit_screen.h"
#include "yuzu_video_core/renderer_vulkan/vk_present_manager.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

BlitScreen::BlitScreen(Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device_,
                       MemoryAllocator& memory_allocator_, PresentManager& present_manager_,
                       Scheduler& scheduler_, const PresentFilters& filters_)
    : device_memory{device_memory_}, device{device_}, memory_allocator{memory_allocator_},
      present_manager{present_manager_}, scheduler{scheduler_}, filters{filters_}, image_count{1},
      swapchain_view_format{VK_FORMAT_B8G8R8A8_UNORM} {}

BlitScreen::~BlitScreen() = default;

void BlitScreen::WaitIdle() {
    present_manager.WaitPresent();
    scheduler.Finish();
    std::scoped_lock lock{device.QueueMutex()};
    device.GetLogical().WaitIdle();
}

void BlitScreen::SetWindowAdaptPass() {
    layers.clear();
    scaling_filter = filters.get_scaling_filter();
    legacy_fsr = VideoCore::LegacyFsr();

    switch (scaling_filter) {
    case ScalingFilter::CatmullRom:
    case ScalingFilter::CatmullRomFast:
        window_adapt = MakeCatmullRom(device, swapchain_view_format);
        break;
    case ScalingFilter::CasNearest:
    case ScalingFilter::CasBilinear:
    case ScalingFilter::CasCatmullRom:
    case ScalingFilter::CasCatmullRomFast:
        window_adapt = MakeCas(device, swapchain_view_format, scaling_filter);
        break;
    case ScalingFilter::NearestNeighbor:
        window_adapt = MakeNearestNeighbor(device, swapchain_view_format);
        break;
    case ScalingFilter::Bicubic:
        window_adapt = MakeBicubic(device, swapchain_view_format);
        break;
    case ScalingFilter::Gaussian:
        window_adapt = MakeGaussian(device, swapchain_view_format);
        break;
    case ScalingFilter::ScaleForceFast:
        window_adapt = MakeScaleForceFast(device, swapchain_view_format);
        break;
    case ScalingFilter::ScaleForce:
        window_adapt = MakeScaleForce(device, swapchain_view_format);
        break;
    case ScalingFilter::Fsr:
        window_adapt = VideoCore::LegacyFsr() ? MakeBilinear(device, swapchain_view_format)
                                             : MakeFsrRcas(device, swapchain_view_format);
        break;
    case ScalingFilter::FsrEasu:
        window_adapt = MakeFsrEasu(device, swapchain_view_format);
        break;
    case ScalingFilter::Bilinear:
    default:
        window_adapt = MakeBilinear(device, swapchain_view_format);
        break;
    }
}

void BlitScreen::DrawToFrame(RasterizerVulkan& rasterizer, Frame* frame,
                             std::span<const Tegra::FramebufferConfig> framebuffers,
                             const Layout::FramebufferLayout& layout,
                             size_t current_swapchain_image_count,
                             VkFormat current_swapchain_view_format) {
    VideoCore::BeginFsrFrame(videoSettings.fsr_sharpening_slider, videoSettings.cas_sharpening_slider,
                            static_cast<unsigned>(videoSettings.scaling_filter), GetDownscalingFilter());
    ScaleCapture::Poll(filters.is_applet_capture);
    bool resource_update_required = false;
    bool presentation_recreate_required = false;

    // Recreate dynamic resources if the adapting filter changed
    if (!window_adapt || scaling_filter != filters.get_scaling_filter() || legacy_fsr != VideoCore::LegacyFsr()) {
        resource_update_required = true;
    }

    // Recreate dynamic resources if the image count changed
    const size_t old_swapchain_image_count =
        std::exchange(image_count, current_swapchain_image_count);
    if (old_swapchain_image_count != current_swapchain_image_count) {
        resource_update_required = true;
    }

    // Recreate the presentation frame if the format or dimensions of the window changed
    const VkFormat old_swapchain_view_format =
        std::exchange(swapchain_view_format, current_swapchain_view_format);
    if (old_swapchain_view_format != current_swapchain_view_format ||
        layout.width != frame->width || layout.height != frame->height) {
        resource_update_required = true;
        presentation_recreate_required = true;
    }

    const VkExtent2D window_size{
        .width = layout.screen.GetWidth(),
        .height = layout.screen.GetHeight(),
    };
    // Every filter may now own an output-sized downscale/sharpen intermediate.
    if (layer_output_size.width != window_size.width ||
        layer_output_size.height != window_size.height) {
        resource_update_required = true;
    }

    // If we have a pending resource update, perform it
    if (resource_update_required) {
        // Wait for idle to ensure no resources are in use
        WaitIdle();

        // Update window adapt pass
        SetWindowAdaptPass();
        layer_output_size = window_size;
        image_index = 0;

        // Update frame format if needed
        if (presentation_recreate_required) {
            present_manager.RecreateFrame(frame, layout.width, layout.height, swapchain_view_format,
                                          window_adapt->GetRenderPass());
        }
    }

    // Add additional layers if needed
    while (layers.size() < framebuffers.size()) {
        layers.emplace_back(device, memory_allocator, scheduler, device_memory, image_count,
                            window_size, window_adapt->GetDescriptorSetLayout(), filters);
    }

    const bool measure = VideoCore::PresentDiagnostics::Enabled();
    VideoCore::PresentDiagnostics::Clock::time_point start;
    if (measure) {
        if (!timer) timer = std::make_unique<PresentTimer>(device, scheduler);
        timer->Begin();
        start = VideoCore::PresentDiagnostics::Clock::now();
    }
    // Perform the draw
    const auto sharpening = filters.is_applet_capture ? PresentSharpening::None
        : GetPresentSharpening(static_cast<ScalingFilter>(VideoCore::fsr_frame_state.selected_filter),
                               VideoCore::fsr_frame_state.cas_strength,
                               VideoCore::fsr_frame_state.sharpness_stored);
    window_adapt->Draw(rasterizer, scheduler, image_index, layers, framebuffers, layout, frame,
                       scaling_filter, sharpening);
    VideoCore::PublishPresentationStatus(filters.is_applet_capture, window_size.width, window_size.height,
                                        static_cast<unsigned>(framebuffers.size()));
    if (measure) {
        auto sample = VideoCore::PresentDiagnostics::Begin(window_size.width, window_size.height,
            static_cast<unsigned>(framebuffers.size()), static_cast<unsigned>(scaling_filter));
        sample.is_applet = filters.is_applet_capture;
        sample.cpu_ms = std::chrono::duration<double, std::milli>(VideoCore::PresentDiagnostics::Clock::now() - start).count();
        timer->End(sample);
    }

    if (!filters.is_applet_capture && ScaleCapture::active)
        ScaleCapture::Output(memory_allocator,scheduler,*frame->image,frame->width,frame->height,swapchain_view_format);

    // Advance to next image
    if (++image_index >= image_count) {
        image_index = 0;
    }
}

vk::Framebuffer BlitScreen::CreateFramebuffer(const Layout::FramebufferLayout& layout,
                                              VkImageView image_view,
                                              VkFormat current_view_format) {
    const bool format_updated =
        std::exchange(swapchain_view_format, current_view_format) != current_view_format;
    if (!window_adapt || scaling_filter != filters.get_scaling_filter() || format_updated) {
        WaitIdle();
        SetWindowAdaptPass();
    }
    const VkExtent2D extent{
        .width = layout.width,
        .height = layout.height,
    };
    return CreateFramebuffer(image_view, extent, window_adapt->GetRenderPass());
}

vk::Framebuffer BlitScreen::CreateFramebuffer(const VkImageView& image_view, VkExtent2D extent,
                                              VkRenderPass render_pass) {
    return device.GetLogical().CreateFramebuffer(VkFramebufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = render_pass,
        .attachmentCount = 1,
        .pAttachments = &image_view,
        .width = extent.width,
        .height = extent.height,
        .layers = 1,
    });
}

} // namespace Vulkan
