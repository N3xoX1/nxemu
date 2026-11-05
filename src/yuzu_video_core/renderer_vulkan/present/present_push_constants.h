// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "yuzu_common/common_types.h"

namespace Vulkan {

struct ScreenRectVertex {
    ScreenRectVertex() = default;
    explicit ScreenRectVertex(f32 x, f32 y, f32 u, f32 v) : position{{x, y}}, tex_coord{{u, v}} {}

    std::array<f32, 2> position;
    std::array<f32, 2> tex_coord;
};

static inline std::array<f32, 4> MakeOrthographicTransform(f32 width, f32 height) {
    return {2.f / width, 2.f / height, -1.f, -1.f};
}

struct PresentPushConstants {
    std::array<f32, 4> transform;
    std::array<ScreenRectVertex, 4> vertices;
    std::array<u32, 12> filter_constants{};
};

static_assert(sizeof(PresentPushConstants) == 128);
static_assert(offsetof(PresentPushConstants, vertices) == 16);
static_assert(offsetof(PresentPushConstants, filter_constants) == 80);
static_assert(sizeof(PresentPushConstants) <= 128, "Push constants are too large");

} // namespace Vulkan
