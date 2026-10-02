// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <limits>
#include "yuzu_common/common_types.h"

namespace Core::Timing {

// Native CPU time, not wall time or calibrated guest cycles. A missing sample
// must not silently become wall time (which would charge waits as CPU work).
std::optional<u64> ReadNativeThreadTimeNs();

// One handler's logical execution across guest fiber switches. Each resumed
// segment starts a new native-thread sample, including after a core migration.
class HleExecutionTime {
public:
    void Begin(std::optional<u64> sample) {
        collecting = true;
        running = false;
        valid = sample.has_value();
        accumulated = 0;
        Resume(sample);
    }
    void Suspend(std::optional<u64> sample) {
        if (!collecting || !running) return;
        valid &= sample.has_value() && *sample >= anchor;
        if (valid) {
            const auto elapsed = *sample - anchor;
            valid = elapsed <= std::numeric_limits<u64>::max() - accumulated;
            if (valid) accumulated += elapsed;
        }
        running = false;
    }
    void Resume(std::optional<u64> sample) {
        if (!collecting || running) return;
        valid &= sample.has_value();
        if (sample) anchor = *sample;
        running = true;
    }
    std::optional<u64> Finish(std::optional<u64> sample) {
        Suspend(sample);
        collecting = false;
        return valid ? std::optional<u64>{accumulated} : std::nullopt;
    }
    bool IsCollecting() const { return collecting; }

private:
    u64 anchor{}, accumulated{};
    bool collecting{}, running{}, valid{};
};

} // namespace Core::Timing
