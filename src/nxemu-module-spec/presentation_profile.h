// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include "nxemu-module-spec/video.h"

inline constexpr bool IsAutomaticScalingFilter(ScalingFilter filter) {
    return filter == ScalingFilter::AutoPerformance || filter == ScalingFilter::AutoQuality;
}

inline constexpr ScalingFilter ManualFilterForProfile(ScalingFilter filter) {
    if (filter == ScalingFilter::AutoQuality) return ScalingFilter::Fsr;
    if (filter == ScalingFilter::AutoPerformance) return ScalingFilter::Bilinear;
    return filter;
}

enum class DefaultProfileGpuKind : uint8_t {
    Unknown,
    Other,
    Integrated,
    Discrete,
};

struct DefaultPresentationProfileInput {
    bool is_mobile{};
    bool is_portable{};
    bool is_software_or_emulated{};
    DefaultProfileGpuKind gpu_kind{DefaultProfileGpuKind::Unknown};
    uint64_t device_local_memory_bytes{};
    bool supports_vulkan_1_2{};
    bool supports_cmaa2_formats{};
    bool supports_fast_fp16{};
};

// Balanced (AutoQuality) adds FSR (EASU + RCAS) and CMAA2/FXAA on top of game rendering.
// Default to Balanced only on desktop systems with a capable discrete GPU that has enough
// dedicated VRAM and the Vulkan format/shader capabilities Balanced relies on; use
// Performance everywhere else (Android, laptops/handhelds, iGPUs, entry-level/legacy dGPUs,
// software/virtual adapters, or unknown hardware).
inline constexpr ScalingFilter DefaultPresentationProfile(
    const DefaultPresentationProfileInput& input) {
    constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
    constexpr uint64_t kMinModernDiscreteVram = 5ULL * kGiB; // 6 GB+ class (OS reserves a slice)
    constexpr uint64_t kMinLegacyDiscreteVram = 7ULL * kGiB; // 8 GB+ class without fast FP16

    if (input.is_mobile || input.is_portable || input.is_software_or_emulated) {
        return ScalingFilter::AutoPerformance;
    }
    if (input.gpu_kind != DefaultProfileGpuKind::Discrete) {
        return ScalingFilter::AutoPerformance;
    }
    if (!input.supports_vulkan_1_2 || !input.supports_cmaa2_formats) {
        return ScalingFilter::AutoPerformance;
    }
    const uint64_t min_vram =
        input.supports_fast_fp16 ? kMinModernDiscreteVram : kMinLegacyDiscreteVram;
    if (input.device_local_memory_bytes < min_vram) {
        return ScalingFilter::AutoPerformance;
    }
    return ScalingFilter::AutoQuality;
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
