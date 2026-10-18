#pragma once
#include <nxemu/ui/i18n/translation.h>
#include <nxemu-module-spec/video.h>

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
inline const ScalingFilterChoice * FindScalingFilter(int value)
{
    for (const ScalingFilterChoice & choice : kScalingFilters)
    {
        if (static_cast<int>(choice.value) == value) return &choice;
    }
    return nullptr;
}
inline constexpr int kSharpnessMax = 200;
