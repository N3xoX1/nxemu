#pragma once
#include <nxemu/ui/i18n/translation.h>
#include <nxemu-module-spec/video.h>
#include <nxemu-module-spec/presentation_profile.h>
#include <nxemu-video/presentation_settings.h>
#include <nxemu-module-spec/fsr_sharpness.h>

struct ScalingFilterChoice
{
    ScalingFilter value;
    const char * label;
    const char * hint;
};
inline constexpr ScalingFilterChoice kScalingFilters[] = {
    {ScalingFilter::AutoPerformance, TrMark("Performance"),
     TrMark("Bicubic upscaling, Area downscaling and no anti-aliasing.")},
    {ScalingFilter::AutoQuality, TrMark("Quality"),
     TrMark("FSR upscaling, Area downscaling and CMAA2 anti-aliasing with an FXAA fallback.")},
    {ScalingFilter::NearestNeighbor, TrMark("Nearest neighbour"), TrMark("Sharp squares, for pixel art. The lightest.")},
    {ScalingFilter::Bilinear, TrMark("Bilinear"), TrMark("Smooth. The usual choice, and light.")},
    {ScalingFilter::CatmullRom, "Catmull-Rom", TrMark("Sharper reconstruction with fine detail. May add halos around strong edges.")},
    {ScalingFilter::CatmullRomFast, TrMark("Catmull-Rom fast"), TrMark("Five-sample approximation. Faster, with small changes around diagonal edges.")},
    {ScalingFilter::Bicubic, TrMark("Bicubic"), TrMark("A little sharper than bilinear, for slightly more work.")},
    {ScalingFilter::ScaleForce, "ScaleForce", TrMark("Sharp edges, best for 2D games and drawings. Heavier.")},
    {ScalingFilter::ScaleForceFast, TrMark("ScaleForce fast"), TrMark("Four-neighbor edge estimate. Faster, but diagonal contours may differ.")},
    {ScalingFilter::Fsr, "FSR", TrMark("AMD FidelityFX Super Resolution: edge-aware upscaling with adjustable sharpness. At 0%, sharpening is off.")},
};
inline const ScalingFilterChoice * FindScalingFilter(int value)
{
    if (value == static_cast<int>(ScalingFilter::FsrEasu)) value = static_cast<int>(ScalingFilter::Fsr);
    for (const ScalingFilterChoice & choice : kScalingFilters)
    {
        if (static_cast<int>(choice.value) == value) return &choice;
    }
    return nullptr;
}
// Of FSR, in percent (display strength, not stored RCAS attenuation).
inline constexpr int kSharpnessMax = FsrSharpness::DisplayMaximum;
