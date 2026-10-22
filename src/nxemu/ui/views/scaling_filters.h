#pragma once
#include <nxemu/ui/i18n/translation.h>
#include <nxemu-module-spec/video.h>

// How the picture of a game is fitted to the window: the ways offered, with
// the name of each and what it does at what cost.
struct ScalingFilterChoice
{
    ScalingFilter value;
    const char * label;
    const char * hint;
};
inline constexpr ScalingFilterChoice kScalingFilters[] = {
    {ScalingFilter::NearestNeighbor, TrMark("Nearest neighbour"), TrMark("Sharp squares, for pixel art. The lightest.")},
    {ScalingFilter::Bilinear, TrMark("Bilinear"), TrMark("Smooth. The usual choice, and light.")},
    {ScalingFilter::Bicubic, TrMark("Bicubic"), TrMark("A little sharper than bilinear, for slightly more work.")},
    {ScalingFilter::ScaleForce, "ScaleForce", TrMark("Sharp edges, best for 2D games and drawings. Heavier.")},
    {ScalingFilter::Fsr, "FSR", TrMark("AMD FidelityFX Super Resolution: sharpens a picture drawn smaller than the window, best for 3D games. Moderate work.")},
};
// The choice a value of the settings is; nullptr for a filter that is no
// longer offered, which the settings of someone may still name.
inline const ScalingFilterChoice * FindScalingFilter(int value)
{
    for (const ScalingFilterChoice & choice : kScalingFilters)
    {
        if (static_cast<int>(choice.value) == value) return &choice;
    }
    return nullptr;
}
// Of FSR, in percent.
inline constexpr int kSharpnessMax = 200;
