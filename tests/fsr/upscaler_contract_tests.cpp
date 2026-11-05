// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/upscaler.h"
#include "yuzu_video_core/renderer_vulkan/present/upscaler.h"
#include <limits>
#include <stdexcept>
using namespace VideoCore::Upscaling;
void Require(bool value) { if (!value) throw std::runtime_error("Upscaler contract regression"); }
int main() {
    const Capabilities spatial{.cpu_synchronized = true};
    FrameParameters frame{{1280, 720}, {1920, 1080}};
    Require(Validate(spatial, frame) == Rejection::None);
    frame.input.width = 0;
    Require(Validate(spatial, frame) == Rejection::EmptyExtent);
    frame.input = {1920, 1080};
    Require(Validate(spatial, frame) == Rejection::NotUpscaling);
    frame.input = {1920, 1200}; // Mixed-axis reduction stays on Area, not a spatial upscaler.
    Require(Validate(spatial, frame) == Rejection::NotUpscaling);
    frame.input = {1280, 720};
    frame.encoding = ColorEncoding::Hdr;
    Require(Validate(spatial, frame) == Rejection::ColorEncoding);
    frame.encoding = ColorEncoding::Linear;
    Require(Validate(spatial, frame) == Rejection::None);
    frame.opaque = false;
    Require(Validate(spatial, frame) == Rejection::Alpha);
    frame.opaque = true;
    // No temporal backend is implemented. This validates the resource requirements
    // that a future adapter must satisfy rather than inventing motion/depth.
    const Capabilities temporal{.temporal = true, .native_resolution = true};
    Require(Validate(temporal, frame) == Rejection::TemporalInputs);
    frame.temporal = TemporalParameters{};
    frame.temporal->has_depth = true;
    frame.temporal->has_motion_vectors = true;
    Require(Validate(temporal, frame) == Rejection::TemporalInputs);
    frame.temporal->has_jitter = true;
    Require(Validate(temporal, frame) == Rejection::None);
    frame.temporal->motion_scale_y = 0;
    Require(Validate(temporal, frame) == Rejection::TemporalInputs);
    frame.temporal->motion_scale_y = -1;
    frame.temporal->jitter_x = std::numeric_limits<float>::quiet_NaN();
    Require(Validate(temporal, frame) == Rejection::TemporalInputs);
    frame.temporal->jitter_x = 0.25f;
    frame.temporal->reset_history = true;
    frame.temporal->motion_units = MotionVectorUnits::Normalized;
    frame.input = frame.output;
    Require(Validate(temporal, frame) == Rejection::None);
    Require(Validate(spatial, frame) == Rejection::NotUpscaling);
    Vulkan::UpscaleFrame resources;
    resources.parameters = frame;
    resources.color = reinterpret_cast<VkImage>(1);
    resources.color_view = reinterpret_cast<VkImageView>(2);
    resources.color_extent = {1920, 1080};
    Require(!Vulkan::IsFrameSupported(temporal, resources));
    resources.depth_view = reinterpret_cast<VkImageView>(3);
    resources.motion_view = reinterpret_cast<VkImageView>(4);
    Require(Vulkan::IsFrameSupported(temporal, resources));
    resources.crop.left = std::numeric_limits<float>::quiet_NaN();
    Require(!Vulkan::IsFrameSupported(temporal, resources));
    resources.crop = {1, 0, 0, 1}; // Flips remain valid; no assumed motion-vector orientation.
    Require(Vulkan::IsFrameSupported(temporal, resources));
    resources.crop.right = resources.crop.left;
    Require(!Vulkan::IsFrameSupported(temporal, resources));
}
