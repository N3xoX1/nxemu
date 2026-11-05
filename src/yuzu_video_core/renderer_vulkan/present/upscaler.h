// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <optional>
#include "yuzu_video_core/upscaler.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"
#include "yuzu_common/math_util.h"

namespace Vulkan {
class Device;
class MemoryAllocator;
class Scheduler;
// All handles stay owned by the caller until the scheduler's resource tick completes.
// A future temporal adapter supplies depth/motion views in addition to their metadata.
struct UpscaleFrame {
    VideoCore::Upscaling::FrameParameters parameters;
    VkImage color{};
    VkExtent2D color_extent{}; // Allocation dimensions; parameters.input is the useful crop.
    VkImageView color_view{}, depth_view{}, motion_view{};
    Common::Rectangle<f32> crop{0, 0, 1, 1};
    size_t image_index{};
};
inline bool IsFrameSupported(const VideoCore::Upscaling::Capabilities& caps, const UpscaleFrame& frame) {
    if (VideoCore::Upscaling::Validate(caps, frame.parameters) != VideoCore::Upscaling::Rejection::None ||
        !frame.color || !frame.color_view || !frame.color_extent.width || !frame.color_extent.height)
        return false;
    if (!std::isfinite(frame.crop.left) || !std::isfinite(frame.crop.right) ||
        !std::isfinite(frame.crop.top) || !std::isfinite(frame.crop.bottom) ||
        frame.crop.left == frame.crop.right || frame.crop.top == frame.crop.bottom)
        return false;
    return !caps.temporal || (frame.depth_view && frame.motion_view);
}
struct UpscaleResult {
    VkImageView view{};
    Common::Rectangle<f32> crop{0, 0, 1, 1};
};
class Upscaler {
public:
    virtual ~Upscaler() = default;
    virtual VideoCore::Upscaling::Backend Backend() const = 0;
    virtual VideoCore::Upscaling::Capabilities Capabilities() const = 0;
    // nullopt means fallback, not a black frame. Never pass partial temporal inputs.
    virtual std::optional<UpscaleResult> Draw(Scheduler&, const UpscaleFrame&) = 0;
};
std::unique_ptr<Upscaler> CreateSpatialUpscaler(VideoCore::Upscaling::Backend backend,
    const Device&, MemoryAllocator&, size_t image_count, VkExtent2D output, bool easu_only = true);
bool RequestMetalFxQuality(); // NXEMU_QUALITY_UPSCALER=metalfx; FSR is the default.
} // namespace Vulkan
