// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>

#include "yuzu_common/math_util.h"
#include "yuzu_common/settings.h"
#include "nxemu-video/video_settings.h"
#include "nxemu-module-spec/fsr_sharpness.h"
#include "nxemu-module-spec/presentation_profile.h"
#include "yuzu_video_core/fsr_diagnostics.h"

static inline ScalingFilter GetScalingFilter() {
    const ScalingFilter upscaler = ManualFilterForProfile(videoSettings.scaling_filter);
    if (videoSettings.cas_sharpening_slider > 0) {
        // The performance preset uses the fast fused reconstruction when CAS is enabled.
        if (videoSettings.scaling_filter == ScalingFilter::AutoPerformance) {
            return ScalingFilter::CasCatmullRomFast;
        }
        switch (upscaler) {
        case ScalingFilter::NearestNeighbor: return ScalingFilter::CasNearest;
        case ScalingFilter::Bilinear: return ScalingFilter::CasBilinear;
        case ScalingFilter::CatmullRom: return ScalingFilter::CasCatmullRom;
        case ScalingFilter::CatmullRomFast: return ScalingFilter::CasCatmullRomFast;
        default: break;
        }
    }
    if (VideoCore::LegacyFsr() && upscaler == ScalingFilter::FsrEasu) {
        return ScalingFilter::Fsr;
    }
    // A true sharpening-off setting must not execute the RCAS shader.
    if (!VideoCore::LegacyFsr() && upscaler == ScalingFilter::Fsr &&
        videoSettings.fsr_sharpening_slider >= FsrSharpness::StoredMaximum) {
        return ScalingFilter::FsrEasu;
    }
    return upscaler;
}

static inline AntiAliasing GetAntiAliasing() {
    return EffectiveProfileAntiAliasing(videoSettings.scaling_filter, videoSettings.renderer_backend,
                                       videoSettings.anti_aliasing);
}

static inline int GetDownscalingFilter() {
    return ProfileDownscalingFilter(videoSettings.scaling_filter, videoSettings.downscaling_filter);
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
    bool is_applet_capture{};
};

constexpr PresentFilters PresentFiltersForDisplay{
    .get_scaling_filter = &GetScalingFilter,
    .get_anti_aliasing = &GetAntiAliasing,
};

constexpr PresentFilters PresentFiltersForAppletCapture{
    .get_scaling_filter = &GetScalingFilterForAppletCapture,
    .get_anti_aliasing = &GetAntiAliasingForAppletCapture,
    .is_applet_capture = true,
};

// RCAS can sample the source directly only when its texel coordinates match
// the full output one-to-one. Full flips are safe: RCAS is symmetric and the
// presentation vertices perform the flip. Partial crops still require EASU.
static inline bool CanUseNativeRcas(u32 input_width, u32 input_height,
                                    const Common::Rectangle<f32>& crop,
                                    u32 output_width, u32 output_height) {
    constexpr f32 epsilon = 0.000001f;
    const bool full_x = (std::abs(crop.left) < epsilon &&
                         std::abs(crop.right - 1.0f) < epsilon) ||
                        (std::abs(crop.right) < epsilon &&
                         std::abs(crop.left - 1.0f) < epsilon);
    const bool full_y = (std::abs(crop.top) < epsilon &&
                         std::abs(crop.bottom - 1.0f) < epsilon) ||
                        (std::abs(crop.bottom) < epsilon &&
                         std::abs(crop.top - 1.0f) < epsilon);
    return input_width != 0 && input_height != 0 && input_width == output_width &&
           input_height == output_height && full_x && full_y;
}

// Classification uses the useful, internally scaled crop, not the allocation size
// or the desktop resolution. A mixed-axis resize uses the downscaler to prefilter
// the shrinking axis. An empty/minimized output never enters an Area loop.
enum class PresentScaleMode : unsigned { Native, Upscale, Downscale };
struct PresentScaleSelection {
    ScalingFilter filter;
    PresentScaleMode mode;
};
// RCAS uses integer texelFetch coordinates. A 1:1 fractional crop still needs
// its subpixel translation materialized before RCAS; no scaling is involved.
static inline bool IsPresentCropTexelAligned(u32 width, u32 height,
                                            const Common::Rectangle<f32>& crop) {
    const double x = double(crop.left) * width;
    const double y = double(crop.top) * height;
    return std::isfinite(x) && std::isfinite(y) &&
           std::abs(x - std::round(x)) <= 0.001 &&
           std::abs(y - std::round(y)) <= 0.001;
}
static inline PresentScaleSelection ResolvePresentScale(
    ScalingFilter upscale, bool area, u32 width, u32 height,
    const Common::Rectangle<f32>& crop, u32 output_width, u32 output_height) {
    const double useful_width = std::abs(double(crop.right) - crop.left) * width;
    const double useful_height = std::abs(double(crop.bottom) - crop.top) * height;
    constexpr double tolerance = 0.001; // Sub-texel roundoff in normalized crops.
    if (!output_width || !output_height || !std::isfinite(useful_width) ||
        !std::isfinite(useful_height) || useful_width <= 0 || useful_height <= 0) {
        return {ScalingFilter::Bilinear, PresentScaleMode::Native};
    }
    if (std::abs(useful_width - output_width) <= tolerance &&
        std::abs(useful_height - output_height) <= tolerance) {
        return {ScalingFilter::Bilinear, PresentScaleMode::Native};
    }
    if (useful_width > output_width + tolerance || useful_height > output_height + tolerance) {
        return {area ? ScalingFilter::Area : ScalingFilter::Bilinear, PresentScaleMode::Downscale};
    }
    return {upscale, PresentScaleMode::Upscale};
}
