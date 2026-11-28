// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <stdexcept>

#include <nxemu-module-spec/base.h>
#include "nxemu-os/os_settings_identifiers.h"
#include "video_settings.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/thread.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_display_bridge.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_display.h"

extern IModuleSettings* g_settings;

namespace Metal {

namespace vk = Vulkan::vk;

namespace {

constexpr VkImageSubresourceRange ColorRange{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

} // Anonymous namespace

DisplayBridge::DisplayBridge(Vulkan::Display& display_, const Device& device_,
                             Scheduler& scheduler_, Core::Frontend::EmuWindow& render_window_)
    : display{display_}, device{device_}, scheduler{scheduler_}, render_window{render_window_},
      use_present_thread{videoSettings.async_presentation} {
    MTL::SharedEvent* const event = scheduler.GetMasterSemaphore().GetEvent();
    const Vulkan::Device& vk_device = display.GetDevice();
    if (!vk_device.IsExtMetalObjectsSupported()) {
        throw std::runtime_error{"The Vulkan device cannot share Metal objects"};
    }
    const vk::Device& logical = vk_device.GetLogical();
    export_metal_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
        display.GetDispatch().vkGetDeviceProcAddr(*logical, "vkExportMetalObjectsEXT"));
    if (export_metal_objects == nullptr) {
        throw std::runtime_error{"vkExportMetalObjectsEXT is missing"};
    }

    // The frames are textures of the Vulkan device, so both sides have to be
    // on the same MTLDevice.
    VkExportMetalDeviceInfoEXT device_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT,
        .pNext = nullptr,
        .mtlDevice = nullptr,
    };
    VkExportMetalObjectsInfoEXT objects{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
        .pNext = &device_info,
    };
    export_metal_objects(*logical, &objects);
    if (static_cast<void*>(device_info.mtlDevice) != static_cast<void*>(device.GetDevice())) {
        throw std::runtime_error{"The Vulkan and Metal renderers use different GPUs"};
    }

    VkImportMetalSharedEventInfoEXT import_event{
        .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT,
        .pNext = nullptr,
        .mtlSharedEvent = reinterpret_cast<MTLSharedEvent_id>(event),
    };
    VkSemaphoreTypeCreateInfo timeline_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = &import_event,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    metal_timeline = logical.CreateSemaphore({
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timeline_info,
        .flags = 0,
    });

    cmdpool = logical.CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = vk_device.GetGraphicsFamily(),
    });
    auto cmdbuffers = cmdpool.Allocate(slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        Vulkan::Frame& frame = slots[i].frame;
        frame.cmdbuf = vk::CommandBuffer{cmdbuffers[i], vk_device.GetDispatchLoader()};
        frame.render_ready = logical.CreateSemaphore({
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        });
        frame.present_done = logical.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        });
        free_frames.push_back(&frame);
    }

    if (use_present_thread) {
        present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
    }
}

DisplayBridge::~DisplayBridge() {
    Release();
    const Vulkan::Device& vk_device = display.GetDevice();
    std::scoped_lock lock{vk_device.QueueMutex()};
    void(vk_device.GetLogical().WaitIdle());
    for (Slot& slot : slots) {
        if (slot.texture != nullptr) {
            device.RemoveResidentAllocation(slot.srgb_view.get());
            device.RemoveResidentAllocation(slot.texture);
        }
    }
    device.CommitResidency();
}

void DisplayBridge::StopPresentThread() {
    {
        std::scoped_lock lock{state_mutex};
        stopping.store(true, std::memory_order_release);
    }
    present_thread.request_stop();
    state_cv.notify_all();
    if (present_thread.joinable()) {
        present_thread.join();
    }
    std::scoped_lock lock{state_mutex};
    while (!present_queue.empty()) {
        free_frames.push_back(present_queue.front().frame);
        present_queue.pop_front();
    }
}

void DisplayBridge::Release() {
    StopPresentThread();
    display.ReleaseFrame();
}

bool DisplayBridge::CanSupersedeQueuedPresent() const {
    if (videoSettings.vsync_mode == VSyncMode::Immediate ||
        videoSettings.vsync_mode == VSyncMode::Mailbox) {
        return true;
    }
    return g_settings != nullptr && !g_settings->GetBool(NXOsSetting::UseSpeedLimit) &&
           g_settings->GetBool(NXOsSetting::UncappedPresent);
}

DisplayBridge::Slot& DisplayBridge::SlotOf(const Vulkan::Frame* frame) {
    return *std::ranges::find_if(slots, [frame](const Slot& slot) { return &slot.frame == frame; });
}

const DisplayBridge::Slot& DisplayBridge::SlotOf(const Vulkan::Frame* frame) const {
    return *std::ranges::find_if(slots, [frame](const Slot& slot) { return &slot.frame == frame; });
}

MTL::Texture* DisplayBridge::Texture(const Vulkan::Frame* frame) const {
    return SlotOf(frame).srgb_view.get();
}

Vulkan::Frame* DisplayBridge::AcquireFrame(u32 width, u32 height) {
    Vulkan::Frame* frame = nullptr;
    u64 metal_tick = 0;
    {
        std::unique_lock lock{state_mutex};
        state_cv.wait(lock, [this] {
            return !free_frames.empty() || stopping.load(std::memory_order_acquire);
        });
        if (free_frames.empty()) {
            return nullptr;
        }
        frame = free_frames.front();
        free_frames.pop_front();
        metal_tick = SlotOf(frame).metal_tick;
    }
    if (frame->present_done.Wait() == VK_ERROR_DEVICE_LOST) {
        display.GetDevice().ReportLoss();
    }
    if (metal_tick != 0) {
        scheduler.Wait(metal_tick);
    }
    if (frame->width != width || frame->height != height || !frame->image) {
        Recreate(SlotOf(frame), width, height);
    }
    return frame;
}

void DisplayBridge::Recreate(Slot& slot, u32 width, u32 height) {
    Vulkan::Frame& frame = slot.frame;
    const Vulkan::Device& vk_device = display.GetDevice();
    const vk::Device& logical = vk_device.GetLogical();
    if (slot.texture != nullptr) {
        // Metal may still draw into the old texture.
        device.RemoveResidentAllocation(slot.srgb_view.get());
        device.RemoveResidentAllocation(slot.texture);
        slot.srgb_view = {};
        slot.texture = nullptr;
    }
    frame.composite_set = VK_NULL_HANDLE;
    frame.composite_pool.reset();
    frame.composite_view.reset();
    frame.image_view.reset();
    frame.width = width;
    frame.height = height;

    const VkFormat format = display.GetSwapchain().GetImageFormat();
    VkExportMetalObjectCreateInfoEXT export_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
        .pNext = nullptr,
        .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
    };
    frame.image = display.GetAllocator().CreateImage(
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &export_info,
            // For the sRGB view the Metal renderer draws through.
            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {.width = width, .height = height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        },
        true);
    frame.image_view = logical.CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *frame.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = {},
        .subresourceRange = ColorRange,
    });

    VkExportMetalTextureInfoEXT texture_info{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
        .pNext = nullptr,
        .image = *frame.image,
        .imageView = VK_NULL_HANDLE,
        .bufferView = VK_NULL_HANDLE,
        .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
        .mtlTexture = nullptr,
    };
    VkExportMetalObjectsInfoEXT objects{
        .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
        .pNext = &texture_info,
    };
    export_metal_objects(*logical, &objects);
    slot.texture = reinterpret_cast<MTL::Texture*>(texture_info.mtlTexture);
    if (slot.texture == nullptr) {
        throw std::runtime_error{"Could not export the frame as a Metal texture"};
    }
    slot.srgb_view = Adopt(slot.texture->newTextureView(MTL::PixelFormatBGRA8Unorm_sRGB));
    if (!slot.srgb_view) {
        throw std::runtime_error{"Could not make an sRGB view of the frame"};
    }
    // Metal 4 reads only what is in the residency set; this texture comes
    // from the Vulkan allocator, which the set does not know about.
    device.AddResidentAllocation(slot.texture);
    device.AddResidentAllocation(slot.srgb_view.get());
    device.CommitResidency();

    // The display expects frames in the GENERAL layout, which Metal has no
    // notion of: nothing to transition.
    display.PrepareFrame(frame, format);
    LOG_INFO(Render_Metal, "Display frame {}x{}", width, height);
}

Vulkan::Frame* DisplayBridge::SubmitAndPresent(
    Vulkan::Frame* frame, u64 tick, const Core::Frontend::EmuWindow::WindowSystemInfo& window) {
    const Vulkan::Device& vk_device = display.GetDevice();
    // Turn the Metal queue's progress into the binary semaphore the display
    // waits for before it reads the frame.
    const VkSemaphore wait = *metal_timeline;
    const VkSemaphore signal = *frame->render_ready;
    const u64 signal_value = 0;
    const VkTimelineSemaphoreSubmitInfo timeline{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &tick,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &signal_value,
    };
    constexpr VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSubmitInfo submit{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &wait,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 0,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &signal,
    };
    {
        std::scoped_lock lock{vk_device.QueueMutex()};
        const VkResult result = vk_device.GetGraphicsQueue().Submit(submit);
        if (result == VK_ERROR_DEVICE_LOST) {
            vk_device.ReportLoss();
        } else {
            vk::Check(result);
        }
    }
    bool presented = false;
    Vulkan::Frame* const reusable = display.Present(frame, window, &presented);
    if (presented) {
        render_window.OnFramePresented(window.render_surface, frame->frame_interval_ns);
    }
    return reusable;
}

void DisplayBridge::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("MetalPresent");
    while (!token.stop_requested()) {
        PendingPresent item{};
        {
            std::unique_lock lock{state_mutex};
            state_cv.wait(lock, [this, &token] {
                return !present_queue.empty() || token.stop_requested() ||
                       stopping.load(std::memory_order_acquire);
            });
            if (token.stop_requested() || stopping.load(std::memory_order_acquire)) {
                break;
            }
            item = present_queue.front();
            present_queue.pop_front();
        }

        ScopedAutoreleasePool pool;
        Vulkan::Frame* reusable = nullptr;
        bool submitted_present = false;
        try {
            reusable = SubmitAndPresent(item.frame, item.tick, item.window);
            submitted_present = true;
        } catch (const std::exception& e) {
            LOG_ERROR(Render_Metal, "DisplayBridge presentation failed: {}", e.what());
            reusable = item.frame;
        }
        // Return the previously retained frame BEFORE waiting on this present's
        // completion so AcquireFrame always has signaled free slots ready.
        if (reusable != nullptr) {
            std::scoped_lock lock{state_mutex};
            free_frames.push_back(reusable);
            state_cv.notify_all();
        }
        // Wait for this present to finish on MoltenVK/CAMetalLayer before popping
        // the next frame from present_queue. This keeps at most 1 present queued on
        // MoltenVKQueue so CAMetalLayer::nextDrawable() never builds a multi-frame
        // backlog, newer Metal frames supersede present_queue.back() in Mailbox mode,
        // and AcquireFrame never stalls on frame->present_done.Wait().
        if (submitted_present) {
            while (!token.stop_requested() && !stopping.load(std::memory_order_acquire)) {
                const VkResult res = item.frame->present_done.Wait(10'000'000ULL);
                if (res == VK_TIMEOUT) {
                    continue;
                }
                if (res == VK_ERROR_DEVICE_LOST) {
                    display.GetDevice().ReportLoss();
                }
                break;
            }
        }
    }
}

void DisplayBridge::Present(Vulkan::Frame* frame, u64 tick,
                            const Core::Frontend::EmuWindow::WindowSystemInfo& window) {
    SlotOf(frame).metal_tick = tick;
    if (!use_present_thread) {
        Vulkan::Frame* const reusable = SubmitAndPresent(frame, tick, window);
        if (reusable != nullptr) {
            std::scoped_lock lock{state_mutex};
            free_frames.push_back(reusable);
        }
        return;
    }

    const bool supersede = CanSupersedeQueuedPresent();
    {
        std::scoped_lock lock{state_mutex};
        if (stopping.load(std::memory_order_acquire)) {
            free_frames.push_back(frame);
            state_cv.notify_all();
            return;
        }
        if (supersede && !present_queue.empty()) {
            // Mailbox semantics: an unpresented frame still sitting in present_queue
            // has never been submitted to MoltenVK or CAMetalLayer, so return it
            // immediately and present only the newer frame instead.
            free_frames.push_back(present_queue.back().frame);
            present_queue.back() = PendingPresent{frame, tick, window};
        } else {
            present_queue.push_back(PendingPresent{frame, tick, window});
        }
    }
    state_cv.notify_all();
}

} // namespace Metal
