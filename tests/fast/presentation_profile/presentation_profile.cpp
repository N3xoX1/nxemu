// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint>
#include <stdexcept>

#include "nxemu-module-spec/presentation_profile.h"

namespace {

constexpr uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

DefaultPresentationProfileInput CapableDesktopDiscrete() {
    DefaultPresentationProfileInput input{};
    input.gpu_kind = DefaultProfileGpuKind::Discrete;
    input.device_local_memory_bytes = 8ULL * kGiB;
    input.supports_vulkan_1_2 = true;
    input.supports_cmaa2_formats = true;
    input.supports_fast_fp16 = true;
    return input;
}

} // namespace

int main() {
    // Performance profile uses Bilinear (never Bicubic or Catmull-Rom fast).
    Require(ManualFilterForProfile(ScalingFilter::AutoPerformance) == ScalingFilter::Bilinear,
            "AutoPerformance manual filter must be Bilinear");
    Require(ManualFilterForProfile(ScalingFilter::AutoQuality) == ScalingFilter::Fsr,
            "AutoQuality manual filter must be FSR");
    Require(ProfileAntiAliasing(ScalingFilter::AutoPerformance, RendererBackend::Vulkan,
                                AntiAliasing::Smaa, true) == AntiAliasing::None,
            "AutoPerformance must disable anti-aliasing");
    Require(ProfileAntiAliasing(ScalingFilter::AutoQuality, RendererBackend::Vulkan,
                                AntiAliasing::None, true) == AntiAliasing::Cmaa2Intel,
            "AutoQuality on Vulkan with CMAA2 must use CMAA2");

    // Default profile heuristic: desktop + capable discrete GPU -> Balanced.
    Require(DefaultPresentationProfile(CapableDesktopDiscrete()) == ScalingFilter::AutoQuality,
            "Desktop with modern 8 GiB discrete GPU should default to Balanced");

    // Modern 6 GB class desktop GPU (OS reserves a small slice, ~5.8 GiB reported) -> Balanced.
    {
        auto input = CapableDesktopDiscrete();
        input.device_local_memory_bytes = 5900ULL * 1024ULL * 1024ULL;
        Require(DefaultPresentationProfile(input) == ScalingFilter::AutoQuality,
                "Modern 6 GB class desktop GPU should default to Balanced");
    }

    // Legacy FP32-only desktop GPU needs 8 GB+ class VRAM for Balanced.
    {
        auto pascal_high = CapableDesktopDiscrete();
        pascal_high.supports_fast_fp16 = false;
        pascal_high.device_local_memory_bytes = 8ULL * kGiB;
        Require(DefaultPresentationProfile(pascal_high) == ScalingFilter::AutoQuality,
                "8 GB FP32 desktop GPU should default to Balanced");

        auto pascal_mid = pascal_high;
        pascal_mid.device_local_memory_bytes = 6ULL * kGiB;
        Require(DefaultPresentationProfile(pascal_mid) == ScalingFilter::AutoPerformance,
                "6 GB FP32-only desktop GPU should default to Performance");
    }

    // Entry-level 4 GB discrete GPU -> Performance.
    {
        auto entry = CapableDesktopDiscrete();
        entry.device_local_memory_bytes = 4ULL * kGiB;
        Require(DefaultPresentationProfile(entry) == ScalingFilter::AutoPerformance,
                "4 GB discrete GPU should default to Performance");
    }

    // Mobile (Android) -> Performance.
    {
        auto mobile = CapableDesktopDiscrete();
        mobile.is_mobile = true;
        Require(DefaultPresentationProfile(mobile) == ScalingFilter::AutoPerformance,
                "Mobile platforms should default to Performance");
    }

    // Portable (laptop / handheld / tablet) -> Performance, even with a discrete GPU.
    {
        auto laptop = CapableDesktopDiscrete();
        laptop.is_portable = true;
        Require(DefaultPresentationProfile(laptop) == ScalingFilter::AutoPerformance,
                "Portable hosts should default to Performance");
    }

    // Integrated GPU (including Apple Silicon / desktop APUs with large unified RAM) -> Performance.
    {
        auto igpu = CapableDesktopDiscrete();
        igpu.gpu_kind = DefaultProfileGpuKind::Integrated;
        igpu.device_local_memory_bytes = 32ULL * kGiB;
        Require(DefaultPresentationProfile(igpu) == ScalingFilter::AutoPerformance,
                "Integrated GPUs should default to Performance");
    }

    // Software, virtual, or translation-layer GPUs -> Performance.
    {
        auto sw = CapableDesktopDiscrete();
        sw.is_software_or_emulated = true;
        Require(DefaultPresentationProfile(sw) == ScalingFilter::AutoPerformance,
                "Software/emulated GPUs should default to Performance");

        auto other = CapableDesktopDiscrete();
        other.gpu_kind = DefaultProfileGpuKind::Other;
        Require(DefaultPresentationProfile(other) == ScalingFilter::AutoPerformance,
                "Non-discrete GPU types should default to Performance");
    }

    // Missing Vulkan 1.2 or CMAA2 required format support -> Performance.
    {
        auto old_vk = CapableDesktopDiscrete();
        old_vk.supports_vulkan_1_2 = false;
        Require(DefaultPresentationProfile(old_vk) == ScalingFilter::AutoPerformance,
                "GPUs below Vulkan 1.2 should default to Performance");

        auto no_cmaa2 = CapableDesktopDiscrete();
        no_cmaa2.supports_cmaa2_formats = false;
        Require(DefaultPresentationProfile(no_cmaa2) == ScalingFilter::AutoPerformance,
                "GPUs lacking CMAA2 format features should default to Performance");
    }

    // Unknown / failed probe -> Performance.
    Require(DefaultPresentationProfile(DefaultPresentationProfileInput{}) ==
                ScalingFilter::AutoPerformance,
            "Unknown hardware should safely default to Performance");

    return 0;
}
