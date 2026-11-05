// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>

// Common user strength: 0 is off, 100 is maximum. Shader-specific responses
// remain different; equal percentages do not promise equal visual results.
class SharpeningStrength {
public:
    static constexpr SharpeningStrength FromPercent(float percent) {
        return SharpeningStrength(std::clamp(percent, 0.0f, 100.0f));
    }
    static constexpr SharpeningStrength FromPercent(int percent) {
        return FromPercent(static_cast<float>(std::clamp(percent, 0, 100)));
    }
    // Compatibility adapter for the historical RCAS configuration key.
    static constexpr SharpeningStrength FromLegacyRcas(int attenuation) {
        return FromPercent(static_cast<float>(200 - std::clamp(attenuation, 0, 200)) * 0.5f);
    }
    constexpr float Percent() const { return percent; }
    constexpr bool IsOff() const { return percent == 0.0f; }
    constexpr float CasUniform() const { return percent / 100.0f; }
    // AMD RCAS expects attenuation in stops, opposite to user strength.
    // A zero strength must also bypass the pass (2 stops alone isn't off).
    constexpr float RcasAttenuationStops() const { return (100.0f - percent) / 50.0f; }
private:
    explicit constexpr SharpeningStrength(float value) : percent(value) {}
    float percent;
};
