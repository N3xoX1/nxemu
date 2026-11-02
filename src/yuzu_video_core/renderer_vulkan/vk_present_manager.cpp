// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_common/thread.h"
#include "frontend/emu_window.h"
#include "yuzu_video_core/renderer_vulkan/vk_display.h"
#include "yuzu_video_core/renderer_vulkan/vk_present_manager.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_swapchain.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

PresentManager::PresentManager(Core::Frontend::EmuWindow& render_window_, const Device& device_,
                               MemoryAllocator& memory_allocator_, Scheduler& scheduler_,
                               Display& display_)
    : render_window{render_window_}, device{device_}, memory_allocator{memory_allocator_},
      scheduler{scheduler_}, display{display_}, swapchain{display_.GetSwapchain()},
      use_present_thread{videoSettings.async_presentation} {
    SetImageCount();

    auto& dld = device.GetLogical();
    cmdpool = dld.CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    auto cmdbuffers = cmdpool.Allocate(image_count);

    frames.resize(image_count);
    for (u32 i = 0; i < frames.size(); i++) {
        Frame& frame = frames[i];
        frame.cmdbuf = vk::CommandBuffer{cmdbuffers[i], device.GetDispatchLoader()};
        frame.render_ready = dld.CreateSemaphore({
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        });
        frame.present_done = dld.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        });
        free_queue.push(&frame);
    }

    if (use_present_thread) {
        present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
    }
}

PresentManager::~PresentManager() {
    present_thread.request_stop();
    if (present_thread.joinable()) {
        present_thread.join();
    }
    // The display may be keeping one of the frames about to be destroyed.
    display.ReleaseFrame();
}

Frame* PresentManager::GetRenderFrame() {
    // Wait for free presentation frames
    std::unique_lock lock{free_mutex};
    free_cv.wait(lock, [this] { return !free_queue.empty(); });

    // Take the frame from the queue
    Frame* frame = free_queue.front();
    free_queue.pop();

    // Wait for the presentation to be finished so all frame resources are free
    frame->present_done.Wait();
    frame->present_done.Reset();

    return frame;
}

void PresentManager::Present(Frame* frame) {
    if (!use_present_thread) {
        scheduler.WaitWorker();
        CopyToSwapchain(frame);
        return;
    }

    scheduler.Record([this, frame](vk::CommandBuffer) {
        std::unique_lock lock{queue_mutex};
        present_queue.push(frame);
        frame_cv.notify_one();
    });
}

void PresentManager::RecreateFrame(Frame* frame, u32 width, u32 height, VkFormat image_view_format,
                                   VkRenderPass rd) {
    auto& dld = device.GetLogical();

    frame->width = width;
    frame->height = height;

    // Views and framebuffers must be released before their old image.
    frame->watermark_framebuffer.reset();
    frame->framebuffer.reset();
    frame->composite_pool.reset();
    frame->composite_view.reset();
    frame->image_view.reset();
    frame->composite_set = VK_NULL_HANDLE;
    frame->image = memory_allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = swapchain.GetImageFormat(),
        .extent =
            {
                .width = width,
                .height = height,
                .depth = 1,
            },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 (display.HasInterface() ? VK_IMAGE_USAGE_SAMPLED_BIT : 0U),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });

    frame->image_view = dld.CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *frame->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_view_format,
        .components =
            {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    });

    const VkImageView image_view{*frame->image_view};
    frame->framebuffer = dld.CreateFramebuffer({
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = rd,
        .attachmentCount = 1,
        .pAttachments = &image_view,
        .width = width,
        .height = height,
        .layers = 1,
    });
    display.PrepareFrame(*frame, swapchain.GetImageFormat());
}

void PresentManager::WaitPresent() {
    if (!use_present_thread) {
        return;
    }

    // Wait for the present queue to be empty
    {
        std::unique_lock queue_lock{queue_mutex};
        frame_cv.wait(queue_lock, [this] { return present_queue.empty(); });
    }

    // The above condition will be satisfied when the last frame is taken from the queue.
    // To ensure that frame has been presented as well take hold of the swapchain
    // mutex.
    std::scoped_lock swapchain_lock{swapchain_mutex};
}

void PresentManager::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("VulkanPresent");
    while (!token.stop_requested()) {
        std::unique_lock lock{queue_mutex};

        // Wait for presentation frames
        Common::CondvarWait(frame_cv, lock, token, [this] { return !present_queue.empty(); });
        if (token.stop_requested()) {
            return;
        }

        // Take the frame and notify anyone waiting
        Frame* frame = present_queue.front();
        present_queue.pop();
        frame_cv.notify_one();

        // By exchanging the lock ownership we take the swapchain lock
        // before the queue lock goes out of scope. This way the swapchain
        // lock in WaitPresent is guaranteed to occur after here.
        std::exchange(lock, std::unique_lock{swapchain_mutex});

        CopyToSwapchain(frame);
    }
}

void PresentManager::SetImageCount() {
    // We cannot have more than 7 images in flight at any given time.
    // FRAMES_IN_FLIGHT is 8, and the cache TICKS_TO_DESTROY is 8.
    // Mali drivers will give us 6.
    image_count = std::min<size_t>(swapchain.GetImageCount(), 7);
}

void PresentManager::CopyToSwapchain(Frame* frame) {
    const auto window = render_window.GetWindowInfo();
    bool presented = false;
    Frame* const reusable = display.Present(frame, window, &presented);
    if (presented) {
        render_window.OnFramePresented(window.render_surface, frame->frame_interval_ns);
    }
    SetImageCount();
    if (reusable != nullptr) {
        std::scoped_lock lock{free_mutex};
        free_queue.push(reusable);
        free_cv.notify_one();
    }
}

} // namespace Vulkan
