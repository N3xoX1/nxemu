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

DeviceMemory::~DeviceMemory() {
    if (memory_maintenance_thread.joinable()) {
        memory_maintenance_thread.request_stop();
        memory_maintenance_thread.join();
    }
}

void DeviceMemory::MaintainGuestMemory(std::stop_token stop) {
#if defined(__linux__) && defined(__GLIBC__) && !defined(__ANDROID__)
    using namespace std::chrono_literals;
    Common::SetCurrentThreadName("GuestMemoryTrim");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    const auto wait_or_stop = [&](std::chrono::milliseconds delay) {
        std::unique_lock lock{trim_wake_mutex};
        trim_wake_cv.wait_for(lock, stop, delay, [] { return false; });
        return stop.stop_requested();
    };
    while (true) {
        if (wait_or_stop(60s)) {
            return;
        }
        // glibc-safe operation; it can contend with other malloc callers,
        // hence the low-priority, opt-in background worker.
        malloc_trim(0);
    }
#else
    (void)stop;
#endif
}

} // namespace Core
