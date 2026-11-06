// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NXEMU_TEST_DISABLE_CMAA2
#undef NXEMU_ENABLE_INTEL_CMAA2
#endif
#include "nxemu-module-spec/presentation_profile.h"
#include "nxemu-video/video_enum_strings.h"
#include "nxemu-video/presentation_settings.h"
#include <string_view>
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/present_sharpening.h"
#include <stdexcept>

VideoSettings videoSettings{};
namespace Settings { bool IsLegacyFsrEnabled() { return false; } }

void Require(bool value) {
    if (!value) throw std::runtime_error("Presentation profile regression");
}

struct SavedSettings {
    int active = static_cast<int>(ScalingFilter::NearestNeighbor);
    int saved = static_cast<int>(ScalingFilter::Bilinear);
    int GetInt(const char* key) const { return std::string_view(key) == NXVideoSetting::SystemScalingFilter ? active : saved; }
    void SetInt(const char* key, int value) { (std::string_view(key) == NXVideoSetting::SystemScalingFilter ? active : saved) = value; }
};
int main() {
    SavedSettings settings;
    SelectPresentationFilter(settings, ScalingFilter::AutoPerformance);
    Require(settings.saved == static_cast<int>(ScalingFilter::NearestNeighbor));
    SelectPresentationFilter(settings, ScalingFilter::AutoQuality);
    Require(settings.saved == static_cast<int>(ScalingFilter::NearestNeighbor));
    ActivateCustomPresentation(settings);
    Require(settings.active == static_cast<int>(ScalingFilter::NearestNeighbor));
    SelectPresentationFilter(settings, ScalingFilter::Fsr);
    SelectPresentationFilter(settings, ScalingFilter::AutoQuality);
    ActivateCustomPresentation(settings);
    Require(settings.active == static_cast<int>(ScalingFilter::Fsr));
    settings.active = static_cast<int>(ScalingFilter::AutoPerformance);
    settings.saved = static_cast<int>(ScalingFilter::Area);
    ActivateCustomPresentation(settings);
    Require(settings.active == static_cast<int>(ScalingFilter::Bilinear));
    SelectPresentationFilter(settings, ScalingFilter::Area);
    Require(settings.active == static_cast<int>(ScalingFilter::Bilinear));

    Require(ScalingFilterFromString("MetalFxSpatial") == ScalingFilter::Fsr);
    settings.active = static_cast<int>(ScalingFilter::AutoQuality);
    settings.saved = 12; // MetalFX Spatial in experimental builds.
    ActivateCustomPresentation(settings);
    Require(settings.active == static_cast<int>(ScalingFilter::Fsr));

    // Existing persisted numeric choices must retain their meanings.
    static_assert(static_cast<int>(ScalingFilter::Fsr) == 5);
    static_assert(static_cast<int>(ScalingFilter::ScaleForceFast) == 9);
    static_assert(static_cast<int>(ScalingFilter::AutoPerformance) == 10);
    static_assert(static_cast<int>(ScalingFilter::AutoQuality) == 11);
    static_assert(static_cast<int>(ScalingFilter::CasNearest) == 16);
    for (const auto profile : {ScalingFilter::AutoPerformance, ScalingFilter::AutoQuality}) {
        Require(ScalingFilterFromString(ScalingFilterToString(profile)) == profile);
        Require(ManualFilterForProfile(profile) == (profile == ScalingFilter::AutoQuality
            ? ScalingFilter::Fsr : ScalingFilter::Bicubic));
        for (const auto backend : {RendererBackend::OpenGL, RendererBackend::Vulkan}) {
            for (const bool compiled : {false, true}) {
                for (const auto saved_aa : {AntiAliasing::None, AntiAliasing::Smaa, AntiAliasing::Fxaa}) {
                    const auto expected = profile == ScalingFilter::AutoPerformance ? AntiAliasing::None :
                        compiled && backend == RendererBackend::Vulkan ? AntiAliasing::Cmaa2Intel : AntiAliasing::Fxaa;
                    Require(ProfileAntiAliasing(profile, backend, saved_aa, compiled) == expected);
                    videoSettings.scaling_filter = profile;
                    videoSettings.renderer_backend = backend;
                    videoSettings.anti_aliasing = saved_aa;
                    Require(GetAntiAliasing() == EffectiveProfileAntiAliasing(profile, backend, saved_aa));
                    Require(GetDownscalingFilter() == 1);
                    Require(videoSettings.anti_aliasing == saved_aa);
                }
            }
        }
    }
    videoSettings.scaling_filter = ScalingFilter::AutoPerformance;
    videoSettings.cas_sharpening_slider = 0;
    Require(GetScalingFilter() == ManualFilterForProfile(ScalingFilter::AutoPerformance));
    videoSettings.cas_sharpening_slider = 35;
    Require(GetScalingFilter() == ScalingFilter::CasCatmullRomFast);
    Require(GetPresentSharpening(ScalingFilter::AutoPerformance, 0, 25) == PresentSharpening::None);
    Require(GetPresentSharpening(ScalingFilter::AutoPerformance, 100, 25) == PresentSharpening::Cas);
    videoSettings.scaling_filter = ScalingFilter::AutoQuality;
    videoSettings.fsr_sharpening_slider = 25;
    Require(GetScalingFilter() == ScalingFilter::Fsr);
    Require(GetPresentSharpening(ScalingFilter::AutoQuality, 100, 25) == PresentSharpening::Rcas);
    videoSettings.fsr_sharpening_slider = FsrSharpness::StoredMaximum;
    Require(GetScalingFilter() == ScalingFilter::FsrEasu);
    Require(GetPresentSharpening(ScalingFilter::AutoQuality, 100, FsrSharpness::StoredMaximum) == PresentSharpening::None);
    // Native and downscaled preset output must select the same scale paths as manual filters.
    const Common::Rectangle<f32> crop{0, 0, 1, 1};
    Require(ResolvePresentScale(GetScalingFilter(), GetDownscalingFilter() == 1, 1280, 720, crop, 640, 360).filter == ScalingFilter::Area);
    for (const auto manual : {ScalingFilter::NearestNeighbor, ScalingFilter::Bicubic, ScalingFilter::Bilinear, ScalingFilter::Fsr, ScalingFilter::CatmullRomFast}) {
        for (const auto aa : {AntiAliasing::None, AntiAliasing::Fxaa, AntiAliasing::Smaa}) {
            Require(ProfileAntiAliasing(manual, RendererBackend::Vulkan, aa, true) == aa);
        }
        Require(ProfileDownscalingFilter(manual, 0) == 0);
        Require(ProfileDownscalingFilter(manual, 1) == 1);
    }
    // Applet captures remain independent of display profiles.
    Require(PresentFiltersForAppletCapture.get_scaling_filter() == ScalingFilter::Bilinear);
    Require(PresentFiltersForAppletCapture.get_anti_aliasing() == AntiAliasing::None);
}
