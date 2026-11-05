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
     TrMark("Favors smoother gameplay. Edges may look more jagged.")},
    {ScalingFilter::AutoQuality, TrMark("Balanced"),
     TrMark("Keeps the picture sharp and smooths jagged edges, with some extra graphics work.")},
    {ScalingFilter::NearestNeighbor, TrMark("Nearest neighbour"), TrMark("Keeps pixels square and crisp. Best for pixel art.")},
    {ScalingFilter::Bilinear, TrMark("Bilinear"), TrMark("Smooths the picture quickly, but fine details can look softer.")},
    {ScalingFilter::CatmullRom, "Catmull-Rom", TrMark("Keeps fine details sharp. Strong edges may show a faint outline.")},
    {ScalingFilter::CatmullRomFast, TrMark("Catmull-Rom fast"), TrMark("A faster version of Catmull-Rom. Diagonal lines may look slightly different.")},
    {ScalingFilter::Bicubic, TrMark("Bicubic"), TrMark("Keeps more detail than Bilinear and uses a little more graphics power.")},
    {ScalingFilter::ScaleForce, "ScaleForce", TrMark("Keeps outlines sharp in 2D games and drawings. Uses more graphics power.")},
    {ScalingFilter::ScaleForceFast, TrMark("ScaleForce fast"), TrMark("A faster version of ScaleForce. Diagonal lines may look slightly different.")},
    {ScalingFilter::Fsr, "FSR", TrMark("Keeps edges clear when making the picture larger. You can adjust how sharp it looks.")},
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
