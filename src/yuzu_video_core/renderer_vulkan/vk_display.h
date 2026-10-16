// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "frontend/emu_window.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/dynamic_library.h"
#include "yuzu_video_core/renderer_vulkan/vk_swapchain.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Frontend {
class GraphicsContext;
}

namespace Vulkan {

struct Frame;

/// Where the game goes in the window, in pixels from its top-left corner.
struct DisplayArea {
    s32 x{};
    s32 y{};
    u32 width{};
    u32 height{};
};

/// The Vulkan device and the window it presents to.
///
/// A display outlives the renderer of a game: it is what shows the frontend's
/// interface while nothing runs. The frontend draws that interface itself, on
/// this device, into an image the display hands it; the display then lays it
/// over the game each time it presents.
class Display {
public:
    using WindowSystemInfo = Core::Frontend::EmuWindow::WindowSystemInfo;

    /// @throw vk::Exception on failure
    Display(const WindowSystemInfo& window, Core::Frontend::GraphicsContext* context, u32 width,
            u32 height);
    ~Display();

    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    const vk::Instance& GetInstance() const {
        return instance;
    }
    const vk::InstanceDispatch& GetDispatch() const {
        return dld;
    }
    const Device& GetDevice() const {
        return device;
    }
    MemoryAllocator& GetAllocator() {
        return memory_allocator;
    }
    const Swapchain& GetSwapchain() const {
        return swapchain;
    }

    /// Attachment is fixed before the game renderer and its threads start.
    bool HasInterface() const {
        return interface_attached;
    }

    /// Presents a frame of the game. Returns the frame the caller may draw
    /// into again: `frame` itself, or an earlier one when the display keeps
    /// `frame` to show it again under the interface.
    Frame* Present(Frame* frame, const WindowSystemInfo& window);

    /// Prepares a newly created game image for sampling by the compositor.
    void PrepareFrame(Frame& frame, VkFormat format);

    /// Gives up the frame kept by Present; call before destroying the frames.
    void ReleaseFrame();

    /// Lets the frontend draw its interface. Until this is called the display
    /// shows the game alone, filling the window. Call before creating the
    /// game renderer so its frame images include sampled-image usage.
    void AttachInterface();

    /// Starts a frame of the interface, `width` by `height` pixels: the size
    /// of the window. Returns the command buffer to record it into and sets
    /// `image` to where it has to end up, an R8G8B8A8_UNORM image with
    /// premultiplied alpha left in the SHADER_READ_ONLY_OPTIMAL layout. The
    /// image starts in TRANSFER_DST_OPTIMAL, safe to overwrite.
    VkCommandBuffer BeginInterfaceFrame(u32 width, u32 height, VkImage& image);

    /// Finishes the frame started by BeginInterfaceFrame and shows it. `area`
    /// is where the game shows through; null hides the game.
    void EndInterfaceFrame(const DisplayArea* area);

    /// Waits for the interface renderer's resources to be safe to release.
    void WaitInterfaceIdle();

private:
    struct InterfaceTarget {
        vk::Image image;
        vk::ImageView view;
        VkDescriptorSet set{};
    };
    struct PresentSlot {
        vk::CommandBuffer cmdbuf;
        vk::Fence done;
    };

    void PresentLocked(Frame* frame, bool fresh);
    void PresentImage(Frame* frame, bool fresh);
    void RecordGameOnly(vk::CommandBuffer cmdbuf, Frame* frame, VkImage image);
    void RecordComposite(vk::CommandBuffer cmdbuf, Frame* frame, VkImage image);
    void RecreateSwapchain(u32 width, u32 height);
    void CreateInterfaceObjects();
    void CreateInterfaceTargets(u32 width, u32 height);
    void CreateCompositePipeline(VkFormat format);
    void WaitOwnPresents();
    bool GameIsPresenting() const;

    std::shared_ptr<Common::DynamicLibrary> library;
    vk::InstanceDispatch dld;
    vk::Instance instance;
    vk::DebugUtilsMessenger debug_messenger;
    vk::SurfaceKHR surface;
    Device device;
    MemoryAllocator memory_allocator;
    Swapchain swapchain;

    /// Guards everything below, and the swapchain.
    std::mutex present_mutex;
    WindowSystemInfo window_info;
    const void* presented_window{};
    bool blit_supported{};

    vk::CommandPool cmdpool;
    /// Used for the presents no frame of the game comes with.
    std::array<PresentSlot, 3> slots;
    std::size_t next_slot{};

    /// The latest frame of the game, kept to present it again.
    Frame* game_frame{};
    std::chrono::steady_clock::time_point last_game_present{};

    bool interface_attached{};
    /// Recorded on the UI thread, independently of the present thread's pool.
    vk::CommandPool interface_cmdpool;
    vk::CommandBuffer interface_cmdbuf;
    vk::Fence interface_done;
    bool interface_submission_pending{};
    vk::Sampler interface_sampler;
    vk::Sampler game_sampler;
    vk::DescriptorSetLayout interface_set_layout;
    vk::DescriptorPool interface_set_pool;
    vk::PipelineLayout composite_layout;
    vk::ShaderModule composite_vertex;
    vk::ShaderModule composite_fragment;
    vk::RenderPass composite_pass;
    vk::Pipeline composite_pipeline;
    vk::Pipeline game_pipeline;
    VkFormat composite_format{VK_FORMAT_UNDEFINED};
    std::vector<vk::ImageView> swapchain_views;
    std::vector<vk::Framebuffer> swapchain_framebuffers;

    /// The interface is drawn into one target while the other is on screen.
    std::array<InterfaceTarget, 2> interface_targets;
    VkExtent2D interface_extent{};
    std::size_t interface_back{};
    bool interface_ready{};
    DisplayArea game_area{};
    bool game_visible{};
    u64 interface_serial{};
    u64 presented_serial{};
    std::condition_variable interface_presented;
};

} // namespace Vulkan
