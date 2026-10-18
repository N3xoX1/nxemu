#pragma once

constexpr struct
{
    const char * label;
    float factor;
} kResolutionScales[] = {
    {"0.5x", 0.5f}, {"0.75x", 0.75f}, {"1x", 1.0f}, {"1.5x", 1.5f}, {"2x", 2.0f}, {"3x", 3.0f},
    {"4x", 4.0f},   {"5x", 5.0f},     {"6x", 6.0f}, {"7x", 7.0f},   {"8x", 8.0f},
};
constexpr int kResolutionScaleCount = static_cast<int>(sizeof(kResolutionScales) / sizeof(kResolutionScales[0]));
constexpr int kDockedHeight = 1080;
constexpr int kHandheldHeight = 720;
