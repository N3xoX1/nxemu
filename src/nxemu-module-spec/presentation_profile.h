// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nxemu-module-spec/video.h"

inline constexpr bool IsAutomaticScalingFilter(ScalingFilter filter) {
    return filter == ScalingFilter::AutoPerformance || filter == ScalingFilter::AutoQuality;
}

inline constexpr ScalingFilter ManualFilterForProfile(ScalingFilter filter) {
    if (filter == ScalingFilter::AutoQuality) return ScalingFilter::Fsr;
    if (filter == ScalingFilter::AutoPerformance) return ScalingFilter::Bicubic;
    return filter;
}

inline constexpr AntiAliasing ProfileAntiAliasing(ScalingFilter filter, RendererBackend backend,
                                                  AntiAliasing manual, bool cmaa2) {
    if (filter == ScalingFilter::AutoPerformance) return AntiAliasing::None;
    if (filter == ScalingFilter::AutoQuality)
        return cmaa2 && backend == RendererBackend::Vulkan ? AntiAliasing::Cmaa2Intel : AntiAliasing::Fxaa;
    return manual;
}

inline constexpr AntiAliasing EffectiveProfileAntiAliasing(ScalingFilter filter,
                                                           RendererBackend backend, AntiAliasing manual) {
#ifdef NXEMU_ENABLE_INTEL_CMAA2
    return ProfileAntiAliasing(filter, backend, manual, true);
#else
    return ProfileAntiAliasing(filter, backend, manual, false);
#endif
}

inline constexpr int ProfileDownscalingFilter(ScalingFilter filter, int manual) {
    return IsAutomaticScalingFilter(filter) ? 1 : manual;
}
