// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <vulkan/vulkan_core.h>
#include "yuzu_common/common_types.h"

namespace Vulkan {
// A lower-left window counts rows up from the bottom of the surface clip. Viewports, scissors
// and the fragment Y coordinate are all mirrored around that height so they stay aligned.
inline void FlipLowerLeftViewportY(float& y, float& height, float clip_height) {
    y = clip_height - y;
    height = -height;
}
inline void FlipLowerLeftScissorY(s32& min_y, s32& max_y, s32 clip_height) {
    const s32 flipped_min = clip_height - max_y;
    max_y = clip_height - min_y;
    min_y = flipped_min;
}
// Pixel-space vertices are normalized against the guest's logical surface clip.
// The corresponding host viewport/scissor must still cover the scaled target.
inline VkViewport SurfaceClipViewport(u32 x, u32 y, u32 width, u32 height,
                                     bool lower_left, float scale) {
    const float host_height = std::max(1.0f, float(height) * scale);
    return VkViewport{
        .x = float(x) * scale,
        .y = float(y) * scale + (lower_left ? host_height : 0.0f),
        .width = std::max(1.0f, float(width) * scale),
        .height = lower_left ? -host_height : host_height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
}
inline VkRect2D SurfaceClipScissor(u32 x, u32 y, u32 width, u32 height,
                                 u32 up_scale, u32 down_shift) {
    // Floor the start and ceil the end so fractional scales cover the whole viewport.
    const u64 round_up = (u64{1} << down_shift) - 1;
    const auto floor_scaled = [=](u64 value) { return u32((value * up_scale) >> down_shift); };
    const auto ceil_scaled = [=](u64 value) {
        return u32((value * up_scale + round_up) >> down_shift);
    };
    const u32 left = floor_scaled(x);
    const u32 top = floor_scaled(y);
    const u32 right = ceil_scaled(u64(x) + width);
    const u32 bottom = ceil_scaled(u64(y) + height);
    return VkRect2D{
        .offset = {static_cast<s32>(left), static_cast<s32>(top)},
        .extent = {std::max(1U, right - left), std::max(1U, bottom - top)},
    };
}
}
