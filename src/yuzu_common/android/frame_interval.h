// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Common::Android {

// Use the requested guest cadence, not the observed FPS of a slow device.
inline uint32_t FrameIntervalNs(uint32_t swap_interval, float speed_scale) {
    if (!(speed_scale > 0.0f)) {
        return 0;
    }
    const double interval = 1'000'000'000.0 * swap_interval / (60.0 * speed_scale);
    return interval >= 1.0 && interval <= 1'000'000'000.0
               ? static_cast<uint32_t>(interval)
               : 0;
}

} // namespace Common::Android
