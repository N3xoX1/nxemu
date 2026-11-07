// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <exception>
#include <utility>

#include "yuzu_common/settings.h"
#include "yuzu_common/logging/log.h"
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
    // Scheduler callbacks capture this manager. Drain them before the
    // presentation thread or its queues can be destroyed.
    try {
        scheduler.WaitWorker();
        WaitPresent();
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Unable to drain presentation jobs: {}", e.what());
    }
    present_thread.request_stop();
    frame_cv.notify_all();
    if (present_thread.joinable()) {
        present_thread.join();
    }
    // The display may be keeping one of the frames about to be destroyed.
    try {
        display.ReleaseFrame();
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Unable to release presentation frame: {}", e.what());
    }
}

Frame* PresentManager::GetRenderFrame() {
    RethrowPresentError();
    Frame* frame;
    {
        // The display hands every frame back, even dropped ones and after a
        // device loss, so only a failed present thread can leave this empty.
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] {
            return !free_queue.empty() || present_failed.load(std::memory_order_acquire);
        });
        RethrowPresentError();
        frame = free_queue.front();
        free_queue.pop();
    }
    // Waiting for the GPU must not hold the free-queue mutex: the present
    // thread needs it to return other frames. The wait is sliced so that a
    // present thread that failed before submitting cannot block us forever.
    while (!present_failed.load(std::memory_order_acquire)) {
        const VkResult result = frame->present_done.Wait(10'000'000ULL);
        if (result == VK_TIMEOUT) {
            continue;
        }
        if (result == VK_ERROR_DEVICE_LOST) {
            // Like the rest of the renderer, keep running without a picture
            // so the user can still stop the emulation.
            device.ReportLoss();
        }
        break;
    }
    RethrowPresentError();
    // The fence is reset just before the next submission that signals it. A
    // dropped frame either leaves it signaled or submits a semaphore drain.
    return frame;
}

void PresentManager::Present(Frame* frame) {
    RethrowPresentError();
    if (!use_present_thread) {
        scheduler.WaitWorker();
        CopyToSwapchain(frame);
        return;
    }

    scheduler.Record([this, frame](vk::CommandBuffer) {
        std::unique_lock lock{queue_mutex};
        present_queue.push(frame);
        // WaitPresent waits on the same condition variable.
        frame_cv.notify_all();
    });
    // Flush() has already dispatched the GPU work. The presentation callback
    // is in a fresh chunk and must not wait for the next emulated frame.
    scheduler.DispatchWork();
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
        frame_cv.wait(queue_lock, [this] {
            return present_queue.empty() || present_failed.load();
        });
    }

    // The above condition will be satisfied when the last frame is taken from the queue.
    // To ensure that frame has been presented as well take hold of the swapchain
    // mutex.
    std::scoped_lock swapchain_lock{swapchain_mutex};
    RethrowPresentError();
}

void PresentManager::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("VulkanPresent");
    try {
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

            // Take the swapchain lock before releasing the queue lock, so
            // WaitPresent cannot finish ahead of this active presentation.
            std::exchange(lock, std::unique_lock{swapchain_mutex});

            CopyToSwapchain(frame);
        }
    } catch (...) {
        present_error = std::current_exception();
        present_failed.store(true, std::memory_order_release);
        frame_cv.notify_all();
        free_cv.notify_all();
    }
}

void PresentManager::RethrowPresentError() const {
    if (present_failed.load(std::memory_order_acquire)) {
        std::rethrow_exception(present_error);
    }
}

void PresentManager::SetImageCount() {
    // We cannot have more than 7 images in flight at any given time.
    // FRAMES_IN_FLIGHT is 8, and the cache TICKS_TO_DESTROY is 8.
    // Mali drivers will give us 6.
    // The interface retains the last game frame while the next one is rendered.
    // A single-image surface must not leave the producer without a free frame.
    image_count = std::clamp<std::size_t>(swapchain.GetImageCount(), 2, 7);
}

void PresentManager::CopyToSwapchain(Frame* frame) {
    const auto window = render_window.GetWindowInfo();
    bool presented = false;
    Frame* const reusable = display.Present(frame, window, &presented);
    if (presented) {
        render_window.OnFramePresented(window.render_surface, frame->frame_interval_ns);
    }
    if (reusable != nullptr) {
        std::scoped_lock lock{free_mutex};
        free_queue.push(reusable);
        free_cv.notify_one();
    }
}

} // namespace Vulkan
