// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace Common {
// Module settings expose readiness across DLL boundaries, but no wait handle.
// Poll only during startup; stop_token wakes cancellation immediately.
template<class Ready>
bool WaitForStartup(std::stop_token stop, Ready&& ready) {
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unique_lock lock(mutex);
    while (!stop.stop_requested()) {
        if (ready()) return true;
        changed.wait_for(lock, stop, std::chrono::milliseconds(20), [] { return false; });
    }
    return false;
}
} // namespace Common
