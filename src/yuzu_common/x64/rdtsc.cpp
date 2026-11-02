// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>

#include "yuzu_common/steady_clock.h"
#include "yuzu_common/uint128.h"
#include "yuzu_common/x64/rdtsc.h"

namespace Common::X64 {

template <u64 Nearest>
static u64 RoundToNearest(u64 value) {
    const auto mod = value % Nearest;
    return mod >= (Nearest / 2) ? (value - mod + Nearest) : (value - mod);
}

u64 detail::CalculateRDTSCFrequency(u64 ticks, s64 elapsed_ns) {
    // A suspended host or a broken timer is not a useful calibration sample.
    if (elapsed_ns <= 0 || elapsed_ns > 10'000'000'000LL || ticks == 0) {
        return 0;
    }
    const u64 timer_diff = static_cast<u64>(elapsed_ns);
    // Check the quotient before integer division (including the MSVC 128-bit quotient limit).
    const double frequency = static_cast<double>(ticks) * 1'000'000'000.0 / timer_diff;
    if (frequency < 1'000'000.0 || frequency > 100'000'000'000.0) {
        return 0;
    }
    return RoundToNearest<100'000>(MultiplyAndDivide64(ticks, 1'000'000'000ULL, timer_diff));
}

u64 EstimateRDTSCFrequency() {
    // Discard the first result measuring the rdtsc.
    FencedRDTSC();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    FencedRDTSC();

    // Get the current time.
    const auto start_time = SteadyClock::Now();
    const u64 tsc_start = FencedRDTSC();
    // Wait for 100 milliseconds.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const auto end_time = SteadyClock::Now();
    const u64 tsc_end = FencedRDTSC();
    // Calculate differences.
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    if (tsc_end <= tsc_start) {
        return 0;
    }
    return detail::CalculateRDTSCFrequency(tsc_end - tsc_start, elapsed);
}

} // namespace Common::X64
