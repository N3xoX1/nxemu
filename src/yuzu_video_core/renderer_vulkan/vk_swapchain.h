// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Vulkan {

class Device;

class Swapchain {
public:
    enum class AcquireResult { Acquired, Outdated, RetryLater };
    explicit Swapchain(VkSurfaceKHR surface, const Device& device, u32 width, u32 height);
    ~Swapchain();

    /// Creates (or recreates) the swapchain with a given size.
    void Create(VkSurfaceKHR surface, u32 width, u32 height);

    /// Acquires an image without blocking the present worker indefinitely.
    AcquireResult AcquireNextImage(u64 timeout_ns);

    /// Presents the rendered image to the swapchain.
    bool Present(VkSemaphore render_semaphore);

    /// Returns true when the swapchain needs to be recreated.
    bool NeedsRecreation() const {
        return IsOutDated() || IsSubOptimal() || NeedsPresentModeUpdate();
    }

    /// Returns true when the swapchain is outdated.
    bool IsOutDated() const {
        return is_outdated;
    }

    /// Returns true when the swapchain is suboptimal.
    bool IsSubOptimal() const {
        return is_suboptimal;
    }

    /// Returns false while the surface cannot hold a swapchain (for example minimized).
    bool IsCreated() const {
        return static_cast<bool>(swapchain);
    }

    VkExtent2D GetSize() const {
        return extent;
    }

    std::size_t GetImageCount() const {
        return image_count;
    }

    std::size_t GetImageIndex() const {
        return image_index;
    }

    std::size_t GetFrameIndex() const {
        return frame_index;
    }

    VkImage GetImageIndex(std::size_t index) const {
        return images[index];
    }

    VkImage CurrentImage() const {
        return images[image_index];
    }

    VkFormat GetImageViewFormat() const {
        return image_view_format;
    }

    VkFormat GetImageFormat() const {
        return surface_format.format;
    }

    VkSemaphore CurrentPresentSemaphore() const {
        return *present_semaphores[frame_index];
    }

    VkSemaphore CurrentRenderSemaphore() const {
        // The next submit waits on this image's acquire semaphore before
        // signaling this semaphore again. A frame fence does not cover present.
        return *render_semaphores[image_index];
    }

    u32 GetWidth() const {
        return width;
    }

    u32 GetHeight() const {
        return height;
    }

    VkExtent2D GetExtent() const {
        return extent;
    }

private:
    void CreateSwapchain(const VkSurfaceCapabilitiesKHR& capabilities);
    void CreateSemaphores();
    void CreateImageViews();

    void Destroy();

    bool NeedsPresentModeUpdate() const;

    VkSurfaceKHR surface;
    const Device& device;

    vk::SwapchainKHR swapchain;

    std::size_t image_count{};
    std::vector<VkImage> images;
    std::vector<vk::Semaphore> present_semaphores;
    std::vector<vk::Semaphore> render_semaphores;

    u32 width;
    u32 height;

    u32 image_index{};
    u32 frame_index{};

    // Known before any swapchain exists: frames can be rendered while the
    // surface cannot hold one yet (for example a window started minimized).
#ifdef ANDROID
    VkFormat image_view_format{VK_FORMAT_R8G8B8A8_UNORM};
#else
    VkFormat image_view_format{VK_FORMAT_B8G8R8A8_UNORM};
#endif
    VkExtent2D extent{};
    VkPresentModeKHR present_mode{};
    VkSurfaceFormatKHR surface_format{};
    bool has_imm{false};
    bool has_mailbox{false};
    bool has_fifo_relaxed{false};

    bool is_outdated{};
    bool is_suboptimal{};
};

} // namespace Vulkan
