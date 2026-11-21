// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>

#if defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)
#include <malloc.h>
#endif

#include "core/device_memory.h"
#include "hle/kernel/board/nintendo/nx/k_system_control.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/thread.h"

namespace Core {

#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
constexpr size_t VirtualReserveSize = 1ULL << 38;
#else
constexpr size_t VirtualReserveSize = 1ULL << 39;
#endif

DeviceMemory::DeviceMemory()
    : buffer{Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize(),
             VirtualReserveSize} {
#ifdef _WIN32
    // Kernel slab/boot allocations precede KMemoryManager's allocation tracking.
    // This commits a small bootstrap region when lazy commitment is enabled.
    constexpr size_t BootstrapSize = 64ULL << 20;
    buffer.CommitBackingRegion(0, std::min(buffer.BackingSize(), BootstrapSize));

    // Lichii #961: make zero-only guest pages leave the resident working set.
    // The pass is not yet proven stable under NXEmu's concurrent mappings:
    // Lichii issue #1769 reports an unresolved crash inside ZeroTrim. Keep it
    // opt-in until Windows long-session tests have been completed.
    const char* const trim = std::getenv("NXEMU_ZERO_TRIM");
    if (trim != nullptr && std::strcmp(trim, "1") == 0) {
        memory_maintenance_thread =
            std::jthread([this](std::stop_token stop) { MaintainGuestMemory(stop); });
    }
#elif defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)
    // glibc malloc_trim can reduce RSS after pipeline and shader cache evictions,
    // but it takes allocator locks. Keep it opt-in until frame-time tests exist.
    const char* const trim = std::getenv("NXEMU_LINUX_HEAP_TRIM");
    if (trim != nullptr && std::strcmp(trim, "1") == 0) {
        memory_maintenance_thread =
            std::jthread([this](std::stop_token stop) { MaintainGuestMemory(stop); });
    }
#endif
}

DeviceMemory::~DeviceMemory() = default;

void DeviceMemory::MaintainGuestMemory(std::stop_token stop) {
#if defined(_WIN32) || (defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__))
    using namespace std::chrono_literals;
    Common::SetCurrentThreadName("GuestMemoryTrim");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    std::mutex mutex;
    std::condition_variable_any wake;
    const auto wait_or_stop = [&](std::chrono::milliseconds delay) {
        std::unique_lock lock{mutex};
        wake.wait_for(lock, stop, delay, [] { return false; });
        return stop.stop_requested();
    };
#ifdef _WIN32
    // Avoid competing with shaders and game data during initial loading.
    std::chrono::seconds interval = 30s;
    // NXEMU_ZERO_TRIM_STRESS=1: a pass every 2 s with no back-off and no wait for bulk mapping,
    // so passes overlap the guest's map/unmap/protect calls (the Lichii #1769 crash window).
    const char* const stress_flag = std::getenv("NXEMU_ZERO_TRIM_STRESS");
    const bool stress = stress_flag != nullptr && std::strcmp(stress_flag, "1") == 0;
    if (stress) {
        interval = 2s;
        LOG_WARNING(HW_Memory, "Zero-page trim stress mode: a pass every 2 s");
    }
    u64 passes = 0;
    u64 trimmed_total = 0;
#else
    std::chrono::seconds interval = 60s;
#endif
    while (true) {
        if (wait_or_stop(interval)) {
            return;
        }
#ifdef _WIN32
        // Lichii #1580: avoid competing with bulk guest map/unmap/protect.
        // Sampling the counter is free unless the optional trim is enabled.
        u64 previously_mapped = buffer.MappedBytes();
        for (int attempt = 0; attempt < (stress ? 0 : 10); ++attempt) {
            if (wait_or_stop(500ms)) {
                return;
            }
            const u64 mapped = buffer.MappedBytes();
            if (mapped - previously_mapped <= (u64{32} << 20)) {
                break;
            }
            previously_mapped = mapped;
        }
        try {
            const auto start = std::chrono::steady_clock::now();
            const auto [resident, zeros] = buffer.TrimZeroPages();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            ++passes;
            trimmed_total += zeros;
            if (stress) {
                if (passes % 30 == 0) {
                    LOG_INFO(HW_Memory,
                             "Zero-page trim stress: {} passes, last {} MiB zero of {} MiB "
                             "resident in {} ms, {} MiB trimmed in total",
                             passes, zeros >> 20, resident >> 20, elapsed, trimmed_total >> 20);
                }
            } else if (zeros < (size_t{64} << 20)) {
                interval = std::min(interval * 2, 300s);
            } else {
                interval = 5s;
                LOG_INFO(HW_Memory,
                         "Zero guest pages: {} MiB out of {} MiB resident, scanned in {} ms",
                         zeros >> 20, resident >> 20, elapsed);
            }
        } catch (const std::exception& e) {
            // Trimming is opportunistic; allocation pressure must never crash the game.
            interval = stress ? 2s : 300s;
            LOG_WARNING(HW_Memory, "Skipping zero-page trim: {}", e.what());
        }
#elif defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)
        // glibc-safe operation; it can contend with other malloc callers,
        // hence the low-priority, opt-in background worker.
        malloc_trim(0);
#endif
    }
#else
    (void)stop;
#endif
}

} // namespace Core
