// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "yuzu_common/common_types.h"

namespace Core::Timing {

// One instance per native thread, not per guest fiber. Reject counter reversal
// or CPU work exceeding elapsed host time. This upper bound does not prove
// conversion accuracy or detect undercounting; startup calibration is retained.
class ThreadTimeValidation {
public:
    bool Observe(u64 cpu, u64 wall_begin, u64 wall_end) {
        if (failed) return false;
        if (wall_end < wall_begin) return Fail();
        if (initialized) {
            if (cpu < previous_cpu || wall_begin < previous_wall_end) return Fail();
            const auto work = cpu - previous_cpu;
            // The counter is sampled within each wall-time window. Preemption
            // between reads must not make valid CPU work exceed the wall bound.
            const auto elapsed = wall_end - previous_wall_begin;
            if (work > elapsed && work - elapsed > elapsed / 10 + 50'000) return Fail();
        }
        previous_cpu = cpu;
        previous_wall_begin = wall_begin;
        previous_wall_end = wall_end;
        initialized = true;
        return true;
    }
private:
    bool Fail() { failed = true; return false; }
    u64 previous_cpu{}, previous_wall_begin{}, previous_wall_end{};
    bool initialized{}, failed{};
};
} // namespace Core::Timing
