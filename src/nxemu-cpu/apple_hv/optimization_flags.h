// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "yuzu_common/common_types.h"
#ifdef NXEMU_APPLE_HV_EXPERIMENTS
#include <chrono>
#include <cstdio>
#include <cstdlib>
#endif
namespace Core::AppleHv {
inline constexpr u32 DirectCleanCache = 1; // Synthetic GPU experiment, excluded from production.
inline constexpr u32 TargetRunningVcpus = 2;
inline constexpr u32 RelaxPermissions = 4;
inline constexpr u32 ResolveFaultImmediately = 8;
inline constexpr u32 DefaultOptimizationFlags = TargetRunningVcpus | RelaxPermissions;
#ifdef NXEMU_APPLE_HV_EXPERIMENTS
// Only hardware-test/benchmark targets permit overrides. Normal builds fold these
// choices at compile time and never read files or environment variables here.
inline u32 OptimizationFlags() {
    static const char* const fixed = std::getenv("NXEMU_APPLE_HV_FLAGS");
    if (fixed) {
        static const u32 value = [] {
            char* end{}; const auto parsed = std::strtoul(fixed, &end, 10);
            return end != fixed && *end == 0 && parsed < 16 ? static_cast<u32>(parsed) : DefaultOptimizationFlags;
        }();
        return value;
    }
    static const char* const path = std::getenv("NXEMU_NATIVE_AB_FILE");
    if (!path) return DefaultOptimizationFlags;
    thread_local auto next = std::chrono::steady_clock::time_point{};
    thread_local u32 mode = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
        next = now + std::chrono::milliseconds(250);
        if (auto* file = std::fopen(path, "r")) {
            if (std::fscanf(file, "%u", &mode) != 1 || mode > 15) mode = DefaultOptimizationFlags;
            std::fclose(file);
        }
    }
    return mode;
}
#else
inline constexpr u32 OptimizationFlags() { return DefaultOptimizationFlags; }
#endif
} // namespace Core::AppleHv
