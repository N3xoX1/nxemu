#pragma once

#include <algorithm>
#include "sharpening_strength.h"

// Keep the saved RCAS attenuation (0..200) unchanged for existing configurations,
// game overrides and status overrides. Only the UI percentage is reversed.
namespace FsrSharpness {
constexpr int DisplayMaximum = 100;
constexpr int StoredMaximum = 200;

constexpr int ToDisplay(int stored) {
    return static_cast<int>(SharpeningStrength::FromLegacyRcas(stored).Percent() + 0.5f);
}

constexpr int ToStored(int display) {
    return StoredMaximum - 2 * std::clamp(display, 0, DisplayMaximum);
}
} // namespace FsrSharpness
