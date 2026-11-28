// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "frontend/emu_window.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/renderer_vulkan/vk_present_manager.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Display;
}

namespace Metal {

class Device;
class Scheduler;

/// Shows the frames of the Metal renderer through the Vulkan display.
///
/// The display owns the window and draws the frontend's interface over the
/// game, so the Metal renderer does not present on its own. Its frames are
/// Vulkan images on the same GPU, exported as Metal textures, and the display
/// waits for the Metal queue through its shared event imported as a timeline
/// semaphore.
///
/// Presentation runs on a dedicated thread with 4 frame slots (1 retained by
/// Display::game_frame for UI compositing, 1 in-flight on MoltenVK/CAMetalLayer,
/// 1 pending latest frame, 1 actively rendered by Metal). In Immediate/Mailbox
/// or uncapped modes, a newer frame supersedes an unpresented pending frame so
/// CAMetalLayer's drawable pacing never stalls the GPU thread with 16-21 ms spikes.
class DisplayBridge {
public:
    /// @throw std::runtime_error when the Vulkan device cannot share its objects.
    DisplayBridge(Vulkan::Display& display, const Device& device, Scheduler& scheduler,
                  Core::Frontend::EmuWindow& render_window);
    ~DisplayBridge();

    DisplayBridge(const DisplayBridge&) = delete;
    DisplayBridge& operator=(const DisplayBridge&) = delete;

    /// Returns a frame to draw into, `width` by `height`, once the display and
    /// Metal queue no longer read it.
    Vulkan::Frame* AcquireFrame(u32 width, u32 height);

    /// The Metal texture of a frame from AcquireFrame.
    [[nodiscard]] MTL::Texture* Texture(const Vulkan::Frame* frame) const;

    /// Shows `frame` once the Metal queue has signalled `tick`.
    void Present(Vulkan::Frame* frame, u64 tick,
                 const Core::Frontend::EmuWindow::WindowSystemInfo& window);

    /// Stops presentation and gives back the frame the display may keep.
    void Release();

private:
    struct Slot {
        Vulkan::Frame frame;
        /// The frame's own texture, owned by the Vulkan image.
        MTL::Texture* texture{};
        /// What the Metal renderer draws into: the presentation blit writes
        /// linear values and expects the target to encode them as sRGB.
        Owned<MTL::Texture> srgb_view;
        /// Last Metal submission tick that wrote to this slot's texture.
        u64 metal_tick{};
    };

    struct PendingPresent {
        Vulkan::Frame* frame{};
        u64 tick{};
        Core::Frontend::EmuWindow::WindowSystemInfo window{};
    };

    void Recreate(Slot& slot, u32 width, u32 height);
    void PresentThread(std::stop_token token);
    void StopPresentThread();
    Vulkan::Frame* SubmitAndPresent(Vulkan::Frame* frame, u64 tick,
                                    const Core::Frontend::EmuWindow::WindowSystemInfo& window);
    [[nodiscard]] bool CanSupersedeQueuedPresent() const;
    [[nodiscard]] Slot& SlotOf(const Vulkan::Frame* frame);
    [[nodiscard]] const Slot& SlotOf(const Vulkan::Frame* frame) const;

    Vulkan::Display& display;
    const Device& device;
    Scheduler& scheduler;
    Core::Frontend::EmuWindow& render_window;
    PFN_vkExportMetalObjectsEXT export_metal_objects{};
    Vulkan::vk::CommandPool cmdpool;
    Vulkan::vk::Semaphore metal_timeline;
    std::array<Slot, 4> slots;
    std::mutex state_mutex;
    std::condition_variable state_cv;
    std::deque<Vulkan::Frame*> free_frames;
    std::deque<PendingPresent> present_queue;
    bool use_present_thread{true};
    std::atomic<bool> stopping{false};
    std::jthread present_thread;
};

} // namespace Metal
