// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>

#include "video_settings.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/host_shaders/blit_color_float_frag_spv.h"
#include "yuzu_video_core/host_shaders/full_screen_triangle_vert_spv.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/renderer_vulkan.h"
#include "yuzu_video_core/renderer_vulkan/vk_display.h"
#include "yuzu_video_core/renderer_vulkan/vk_present_manager.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/vulkan_common/vulkan_debug_callback.h"
#include "yuzu_video_core/vulkan_common/vulkan_instance.h"
#include "yuzu_video_core/vulkan_common/vulkan_library.h"
#include "yuzu_video_core/vulkan_common/vulkan_surface.h"

namespace Vulkan {

namespace {

constexpr VkFormat InterfaceFormat = VK_FORMAT_R8G8B8A8_UNORM;

constexpr std::chrono::milliseconds GameFrameTimeout{100};
constexpr std::chrono::milliseconds InterfaceFrameTimeout{50};

constexpr VkImageSubresourceRange ColorRange{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

constexpr VkImageSubresourceLayers ColorLayers{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .mipLevel = 0,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

/// Push constants of full_screen_triangle.vert.
struct TriangleConstants {
    std::array<float, 2> tex_scale{1.0f, 1.0f};
    std::array<float, 2> tex_offset{0.0f, 0.0f};
};

bool CanBlitToSwapchain(const vk::PhysicalDevice& physical_device, VkFormat format) {
    const VkFormatProperties props{physical_device.GetFormatProperties(format)};
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
}

VkImageMemoryBarrier Barrier(VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access,
                             VkImageLayout old_layout, VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = ColorRange,
    };
}

VkOffset3D Corner(s32 x, s32 y) {
    return VkOffset3D{.x = x, .y = y, .z = 0};
}

VkOffset3D FarCorner(s32 x, s32 y) {
    return VkOffset3D{.x = x, .y = y, .z = 1};
}

} // Anonymous namespace

Display::Display(const WindowSystemInfo& window, Core::Frontend::GraphicsContext* context,
                 u32 width, u32 height)
    : library(OpenLibrary(context)),
      instance(CreateInstance(*library, dld, VK_API_VERSION_1_1, window.type,
                              videoSettings.renderer_debug)),
      debug_messenger(videoSettings.renderer_debug ? CreateDebugUtilsCallback(instance)
                                                   : vk::DebugUtilsMessenger{}),
      surface(CreateSurface(instance, window)), device(CreateDevice(instance, dld, *surface)),
      memory_allocator(device), swapchain(*surface, device, width, height), window_info(window),
      presented_window(window.render_surface),
      blit_supported(CanBlitToSwapchain(device.GetPhysical(), swapchain.GetImageViewFormat())) {
    const auto& logical = device.GetLogical();
    cmdpool = logical.CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    const auto cmdbufs = cmdpool.Allocate(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i].cmdbuf = vk::CommandBuffer{cmdbufs[i], device.GetDispatchLoader()};
        slots[i].done = logical.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        });
    }
}

Display::~Display() {
    std::scoped_lock lock{present_mutex, device.QueueMutex()};
    void(device.GetLogical().WaitIdle());
}

Frame* Display::Present(Frame* frame, const WindowSystemInfo& window) {
    std::scoped_lock lock{present_mutex};
    window_info = window;

    Frame* reusable = frame;
    if (interface_attached) {
        reusable = std::exchange(game_frame, frame);
        if (reusable != nullptr) {
            // Presents that came without a frame of the game may still read it.
            WaitOwnPresents();
        }
    }
    PresentLocked(frame, true);
    last_game_present = std::chrono::steady_clock::now();
    return reusable;
}

void Display::ReleaseFrame() {
    std::scoped_lock lock{present_mutex};
    if (game_frame != nullptr) {
        WaitOwnPresents();
        game_frame = nullptr;
    }
}

void Display::AttachInterface() {
    std::scoped_lock lock{present_mutex};
    if (!interface_attached) {
        CreateInterfaceObjects();
        interface_attached = true;
    }
}

VkCommandBuffer Display::BeginInterfaceFrame(u32 width, u32 height, VkImage& image) {
    // The previous frame of the interface has to be done before its command
    // buffer is recorded again.
    WaitInterfaceIdle();

    if (interface_extent.width != width || interface_extent.height != height) {
        std::scoped_lock lock{present_mutex};
        CreateInterfaceTargets(width, height);
    }
    image = *interface_targets[interface_back].image;
    interface_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    // Discard the old contents only after earlier composites have finished
    // sampling this back target. Queue submission order alone does not order
    // fragment reads against transfer writes in a later submission.
    interface_cmdbuf.PipelineBarrier(
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        Barrier(image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL));
    return *interface_cmdbuf;
}

void Display::WaitInterfaceIdle() {
    if (interface_submission_pending) {
        interface_done.Wait();
        interface_submission_pending = false;
    }
}

void Display::EndInterfaceFrame(const DisplayArea* area) {
    interface_cmdbuf.End();
    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = interface_cmdbuf.address(),
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    {
        std::scoped_lock lock{device.QueueMutex()};
        // Leave the fence signalled if starting/recording the frame fails.
        interface_done.Reset();
        vk::Check(device.GetGraphicsQueue().Submit(submit_info, *interface_done));
        // A failed submit leaves the reset fence unsignalled. Teardown must
        // only wait for work that was actually submitted successfully.
        interface_submission_pending = true;
    }

    // Presents recorded from here on read the frame just submitted, and the
    // queue runs them after it.
    std::unique_lock lock{present_mutex};
    interface_back = 1 - interface_back;
    interface_ready = true;
    game_visible = area != nullptr;
    if (area != nullptr) {
        game_area = *area;
    }
    const u64 serial = ++interface_serial;

    if (GameIsPresenting() &&
        interface_presented.wait_for(lock, InterfaceFrameTimeout,
                                     [this, serial] { return presented_serial >= serial; })) {
        return;
    }
    PresentLocked(game_frame, false);
}

bool Display::GameIsPresenting() const {
    return game_frame != nullptr &&
           std::chrono::steady_clock::now() - last_game_present < GameFrameTimeout;
}

void Display::WaitOwnPresents() {
    for (const PresentSlot& slot : slots) {
        slot.done.Wait();
    }
}

void Display::PresentLocked(Frame* frame, bool fresh) {
    const void* window = window_info.render_surface;
    if (window == nullptr) {
        presented_window = nullptr;
        return;
    }
    if (frame == nullptr && !interface_ready) {
        return;
    }

    try {
        if (window != presented_window) {
            surface = CreateSurface(instance, window_info);
            presented_window = window;
            RecreateSwapchain(swapchain.GetWidth(), swapchain.GetHeight());
        }
        PresentImage(frame, fresh);
    } catch (const vk::Exception& except) {
        if (except.GetResult() != VK_ERROR_SURFACE_LOST_KHR &&
            except.GetResult() != VK_ERROR_INITIALIZATION_FAILED) {
            throw;
        }
        presented_window = nullptr;
    }
}

void Display::PresentImage(Frame* frame, bool fresh) {
    // With an interface the swapchain covers the window; alone, the game is
    // the only thing in it and its frames set the size.
    const bool composite = interface_attached && interface_extent.width != 0;
    const u32 width = composite ? interface_extent.width : frame->width;
    const u32 height = composite ? interface_extent.height : frame->height;

    if (swapchain.NeedsRecreation() || swapchain.GetWidth() != width ||
        swapchain.GetHeight() != height) {
        RecreateSwapchain(width, height);
    }
    while (swapchain.AcquireNextImage()) {
        RecreateSwapchain(width, height);
    }

    PresentSlot* slot = nullptr;
    if (!fresh) {
        slot = &slots[next_slot];
        next_slot = (next_slot + 1) % slots.size();
        slot->done.Wait();
        slot->done.Reset();
    }
    const vk::CommandBuffer cmdbuf{fresh ? frame->cmdbuf : slot->cmdbuf};
    cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    if (composite) {
        RecordComposite(cmdbuf, frame, swapchain.CurrentImage());
    } else {
        RecordGameOnly(cmdbuf, frame, swapchain.CurrentImage());
    }
    cmdbuf.End();

    const VkSemaphore present_semaphore = swapchain.CurrentPresentSemaphore();
    const VkSemaphore render_semaphore = swapchain.CurrentRenderSemaphore();
    // Only the first present of a frame waits for the game to finish drawing it.
    const std::array wait_semaphores{present_semaphore,
                                     fresh ? *frame->render_ready : VkSemaphore{}};
    static constexpr std::array<VkPipelineStageFlags, 2> wait_stage_masks{
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
    };
    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = fresh ? 2U : 1U,
        .pWaitSemaphores = wait_semaphores.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = cmdbuf.address(),
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &render_semaphore,
    };
    {
        std::scoped_lock submit_lock{device.QueueMutex()};
        const VkFence done = fresh ? *frame->present_done : *slot->done;
        switch (const VkResult result = device.GetGraphicsQueue().Submit(submit_info, done)) {
        case VK_SUCCESS:
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
    }
    swapchain.Present(render_semaphore);

    presented_serial = interface_serial;
    interface_presented.notify_all();
}

void Display::RecordGameOnly(vk::CommandBuffer cmdbuf, Frame* frame, VkImage image) {
    const VkExtent2D extent = swapchain.GetExtent();
    const std::array pre_barriers{
        Barrier(image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        Barrier(*frame->image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
    };
    const std::array post_barriers{
        Barrier(image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR),
        Barrier(*frame->image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };

    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, {},
                           {}, {}, pre_barriers);
    if (blit_supported) {
        cmdbuf.BlitImage(
            *frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VkImageBlit{
                .srcSubresource = ColorLayers,
                .srcOffsets = {Corner(0, 0), FarCorner(static_cast<s32>(frame->width),
                                                       static_cast<s32>(frame->height))},
                .dstSubresource = ColorLayers,
                .dstOffsets = {Corner(0, 0), FarCorner(static_cast<s32>(extent.width),
                                                       static_cast<s32>(extent.height))},
            },
            VK_FILTER_LINEAR);
    } else {
        cmdbuf.CopyImage(*frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VkImageCopy{
                             .srcSubresource = ColorLayers,
                             .srcOffset = Corner(0, 0),
                             .dstSubresource = ColorLayers,
                             .dstOffset = Corner(0, 0),
                             .extent{
                                 .width = std::min(frame->width, extent.width),
                                 .height = std::min(frame->height, extent.height),
                                 .depth = 1,
                             },
                         });
    }
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, {},
                           {}, {}, post_barriers);
}

void Display::RecordComposite(vk::CommandBuffer cmdbuf, Frame* frame, VkImage image) {
    const VkExtent2D extent = swapchain.GetExtent();
    const s32 right = static_cast<s32>(extent.width);
    const s32 bottom = static_cast<s32>(extent.height);
    const s32 x0 = std::clamp(game_area.x, 0, right);
    const s32 y0 = std::clamp(game_area.y, 0, bottom);
    const s32 x1 = std::clamp(game_area.x + static_cast<s32>(game_area.width), 0, right);
    const s32 y1 = std::clamp(game_area.y + static_cast<s32>(game_area.height), 0, bottom);
    const bool draw_game = frame != nullptr && game_visible && x1 > x0 && y1 > y0;
    if (draw_game) {
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
            Barrier(*frame->image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    }

    const VkFormat format = swapchain.GetImageFormat();
    if (format != composite_format) {
        CreateCompositePipeline(format);
        swapchain_framebuffers.clear();
        swapchain_views.clear();
    }
    const std::size_t index = swapchain.GetImageIndex();
    if (swapchain_framebuffers.size() <= index) {
        swapchain_views.resize(swapchain.GetImageCount());
        swapchain_framebuffers.resize(swapchain.GetImageCount());
    }
    if (!swapchain_framebuffers[index]) {
        const auto& logical = device.GetLogical();
        swapchain_views[index] = logical.CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components{},
            .subresourceRange = ColorRange,
        });
        const VkImageView view = *swapchain_views[index];
        swapchain_framebuffers[index] = logical.CreateFramebuffer({
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderPass = *composite_pass,
            .attachmentCount = 1,
            .pAttachments = &view,
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
        });
    }

    // Clear, scale the game and blend the UI in one attachment pass. This
    // avoids a transfer into the swapchain followed by reloading it for UI.
    const VkClearValue clear{.color = {.float32 = {0.0f, 0.0f, 0.0f, 1.0f}}};
    cmdbuf.BeginRenderPass(VkRenderPassBeginInfo{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = *composite_pass,
        .framebuffer = *swapchain_framebuffers[index],
        .renderArea = {{0, 0}, extent},
        .clearValueCount = 1,
        .pClearValues = &clear,
    }, VK_SUBPASS_CONTENTS_INLINE);
    if (draw_game) {
        const VkViewport viewport{static_cast<float>(x0), static_cast<float>(y0),
                                  static_cast<float>(x1 - x0), static_cast<float>(y1 - y0), 0.0f, 1.0f};
        const VkRect2D scissor{{x0, y0}, {static_cast<u32>(x1 - x0), static_cast<u32>(y1 - y0)}};
        const TriangleConstants constants{
            .tex_scale = {static_cast<float>(x1 - x0) / game_area.width,
                          static_cast<float>(y1 - y0) / game_area.height},
            .tex_offset = {static_cast<float>(x0 - game_area.x) / game_area.width,
                           static_cast<float>(y0 - game_area.y) / game_area.height},
        };
        cmdbuf.SetViewport(0, viewport);
        cmdbuf.SetScissor(0, scissor);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *game_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *composite_layout, 0,
                                  frame->composite_set, {});
        cmdbuf.PushConstants(*composite_layout, VK_SHADER_STAGE_VERTEX_BIT, constants);
        cmdbuf.Draw(3, 1, 0, 0);
    }
    if (interface_ready) {
        cmdbuf.SetViewport(0, VkViewport{0, 0, static_cast<float>(extent.width),
                                        static_cast<float>(extent.height), 0, 1});
        cmdbuf.SetScissor(0, VkRect2D{{0, 0}, extent});
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *composite_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *composite_layout, 0,
                                  interface_targets[1 - interface_back].set, {});
        cmdbuf.PushConstants(*composite_layout, VK_SHADER_STAGE_VERTEX_BIT, TriangleConstants{});
        cmdbuf.Draw(3, 1, 0, 0);
    }
    cmdbuf.EndRenderPass();
    if (draw_game) {
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            Barrier(*frame->image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL));
    }
}

void Display::RecreateSwapchain(u32 width, u32 height) {
    if (!swapchain_framebuffers.empty()) {
        // Earlier presents may still be drawing into the views.
        std::scoped_lock lock{device.QueueMutex()};
        void(device.GetLogical().WaitIdle());
    }
    swapchain_framebuffers.clear();
    swapchain_views.clear();
    swapchain.Create(*surface, width, height);
}

void Display::CreateInterfaceObjects() {
    const auto& logical = device.GetLogical();
    interface_cmdpool = logical.CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    interface_cmdbuf =
        vk::CommandBuffer{interface_cmdpool.Allocate(1)[0], device.GetDispatchLoader()};
    interface_done = logical.CreateFence({
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    });
    interface_sampler = CreateNearestNeighborSampler(device);
    game_sampler = CreateWrappedSampler(device);
    interface_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    interface_set_pool =
        CreateWrappedDescriptorPool(device, interface_targets.size(), interface_targets.size(),
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    const std::array layouts{*interface_set_layout, *interface_set_layout};
    const vk::DescriptorSets sets = CreateWrappedDescriptorSets(
        interface_set_pool, vk::Span<VkDescriptorSetLayout>(layouts.data(), layouts.size()));
    for (std::size_t i = 0; i < interface_targets.size(); ++i) {
        interface_targets[i].set = sets[i];
    }

    constexpr VkPushConstantRange constants{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(TriangleConstants),
    };
    composite_layout = logical.CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = interface_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &constants,
    });
    composite_vertex = BuildShader(device, FULL_SCREEN_TRIANGLE_VERT_SPV);
    composite_fragment = BuildShader(device, BLIT_COLOR_FLOAT_FRAG_SPV);
}

void Display::PrepareFrame(Frame& frame, VkFormat format) {
    if (!interface_attached) {
        return;
    }
    // Sample the base image format, matching the old image blit even when
    // the game's rendering view uses a different sRGB interpretation.
    frame.composite_view = CreateWrappedImageView(device, frame.image, format);
    frame.composite_pool = CreateWrappedDescriptorPool(device, 1, 1);
    const std::array layouts{*interface_set_layout};
    frame.composite_set = CreateWrappedDescriptorSets(
        frame.composite_pool, vk::Span<VkDescriptorSetLayout>(layouts.data(), layouts.size()))[0];
    const VkDescriptorImageInfo image_info{
        .sampler = *game_sampler,
        .imageView = *frame.composite_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    device.GetLogical().UpdateDescriptorSets(VkWriteDescriptorSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = frame.composite_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    }, {});
}

void Display::CreateInterfaceTargets(u32 width, u32 height) {
    const auto& logical = device.GetLogical();
    {
        // Presents still running may read the targets being replaced.
        std::scoped_lock lock{device.QueueMutex()};
        void(logical.WaitIdle());
    }
    interface_ready = false;
    interface_back = 0;
    interface_extent = VkExtent2D{.width = width, .height = height};

    for (InterfaceTarget& target : interface_targets) {
        target.view.reset();
        // These two long-lived images should not open a large block in the
        // game's allocator while the frontend is idle. Dedicated memory also
        // returns their old storage on resize instead of retaining pool slack.
        target.image = memory_allocator.CreateImage({
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = InterfaceFormat,
            .extent{.width = width, .height = height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        }, true);
        target.view = logical.CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = *target.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = InterfaceFormat,
            .components{},
            .subresourceRange = ColorRange,
        });
        const VkDescriptorImageInfo image_info{
            .sampler = *interface_sampler,
            .imageView = *target.view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = target.set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_info,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        };
        logical.UpdateDescriptorSets(write, {});
    }
}

void Display::CreateCompositePipeline(VkFormat format) {
    const VkAttachmentDescription attachment{
        .flags = 0,
        .format = format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    constexpr VkAttachmentReference color_reference{
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    const VkSubpassDescription subpass{
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference,
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = nullptr,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };
    composite_pipeline.reset();
    game_pipeline.reset();
    composite_pass = device.GetLogical().CreateRenderPass({
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    });
    // The window surface is opaque content even when its Metal layer opts out
    // of direct-to-display. Keep the alpha of the opaque clear: neither guest
    // framebuffer alpha nor transparent UI pixels should hide the final image.
    composite_pipeline = CreateWrappedPremultipliedBlendingPipeline(
        device, composite_pass, composite_layout, std::tie(composite_vertex, composite_fragment),
        false);
    game_pipeline = CreateWrappedPipeline(
        device, composite_pass, composite_layout, std::tie(composite_vertex, composite_fragment),
        false);
    composite_format = format;
}

} // namespace Vulkan
