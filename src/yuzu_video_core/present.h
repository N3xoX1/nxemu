// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_common/settings.h"
#include "video_settings.h"

static inline ScalingFilter GetScalingFilter() {
    return videoSettings.scaling_filter.GetValue();
}

static inline AntiAliasing GetAntiAliasing() {
    return videoSettings.anti_aliasing.GetValue();
}

static inline ScalingFilter GetScalingFilterForAppletCapture() {
    return ScalingFilter::Bilinear;
}

static inline AntiAliasing GetAntiAliasingForAppletCapture() {
    return AntiAliasing::None;
}

struct PresentFilters {
    ScalingFilter (*get_scaling_filter)();
    AntiAliasing (*get_anti_aliasing)();
};

constexpr PresentFilters PresentFiltersForDisplay{
    .get_scaling_filter = &GetScalingFilter,
    .get_anti_aliasing = &GetAntiAliasing,
};

constexpr PresentFilters PresentFiltersForAppletCapture{
    .get_scaling_filter = &GetScalingFilterForAppletCapture,
    .get_anti_aliasing = &GetAntiAliasingForAppletCapture,
};
