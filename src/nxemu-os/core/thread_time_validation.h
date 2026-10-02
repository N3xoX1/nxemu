// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "yuzu_common/common_types.h"

namespace Core::Timing {

// One instance per native thread, not per guest fiber. Reject counter reversal
// or CPU work exceeding elapsed host time. This upper bound does not prove
// conversion accuracy or detect undercounting; startup calibration is retained.
class ThreadTimeValidation {
public:
    bool Observe(u64 cpu, u64 wall) {
        if (failed) return false;
        if (initialized) {
            if (cpu < previous_cpu || wall < previous_wall) return Fail();
            const auto work = cpu - previous_cpu;
            const auto elapsed = wall - previous_wall;
            // QPC and the CPU counter are sampled separately. Leave absolute
            // noise room; tiny handlers must not fail due to sampling overhead.
            if (work > elapsed && work - elapsed > elapsed / 10 + 50'000) return Fail();
        }
        previous_cpu = cpu;
        previous_wall = wall;
        initialized = true;
        return true;
    }
private:
    bool Fail() { failed = true; return false; }
    u64 previous_cpu{}, previous_wall{};
    bool initialized{}, failed{};
};
} // namespace Core::Timing
