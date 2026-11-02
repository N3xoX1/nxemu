// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "yuzu_common/common_types.h"

namespace Common::X64 {

#ifdef _MSC_VER
__forceinline static u64 FencedRDTSC() {
    _mm_lfence();
    _ReadWriteBarrier();
    const u64 result = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return result;
}
#else
static inline u64 FencedRDTSC() {
    u64 eax;
    u64 edx;
    asm volatile("lfence\n\t"
                 "rdtsc\n\t"
                 "lfence\n\t"
                 : "=a"(eax), "=d"(edx));
    return (edx << 32) | eax;
}
#endif

// Deliberately broad bounds: reject corrupt measurements without tying this to a CPU model.
constexpr bool IsValidTSCFrequency(u64 frequency) {
    return frequency >= 1'000'000 && frequency <= 100'000'000'000ULL;
}

u64 EstimateRDTSCFrequency();

namespace detail {
u64 CalculateRDTSCFrequency(u64 ticks, s64 elapsed_ns);
}

} // namespace Common::X64
