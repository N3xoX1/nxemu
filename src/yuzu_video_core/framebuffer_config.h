// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_common/common_types.h"
#include "yuzu_common/fence.h"
#include "yuzu_common/math_util.h"
#include "yuzu_common/nvdata.h"

namespace Tegra {

/**
 * Struct describing framebuffer configuration
 */
struct FramebufferConfig {
    DAddr address{};
    u32 offset{};
    u32 width{};
    u32 height{};
    u32 stride{};
    Service::android::PixelFormat pixel_format{};
    Service::android::BufferTransformFlags transform_flags{};
    Common::Rectangle<int> crop_rect{};
    BlendMode blending{};
    u32 frame_interval_ns{};
};

Common::Rectangle<f32> NormalizeCrop(const FramebufferConfig& framebuffer, u32 texture_width,
                                     u32 texture_height);

} // namespace Tegra
