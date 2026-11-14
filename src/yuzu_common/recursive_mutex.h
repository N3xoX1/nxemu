// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <shared_mutex>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include "yuzu_common/common_types.h"

namespace Common {

/// Recursive mutex for locks taken many times per frame with little contention.
/// The owner check avoids the thread id lookup and bookkeeping of std::recursive_mutex.
/// The inner lock is std::shared_mutex, which MSVC implements as a plain SRW lock.
class RecursiveMutex {
public:
    RecursiveMutex() = default;
    RecursiveMutex(const RecursiveMutex&) = delete;
    RecursiveMutex& operator=(const RecursiveMutex&) = delete;

    void lock() {
        const u32 self = ThisThread();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return;
        }
        if (!inner.try_lock()) {
            waiters.fetch_add(1, std::memory_order_relaxed);
            inner.lock();
            waiters.fetch_sub(1, std::memory_order_relaxed);
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
    }

    bool try_lock() {
        const u32 self = ThisThread();
        if (owner.load(std::memory_order_relaxed) == self) {
            ++depth;
            return true;
        }
        if (!inner.try_lock()) {
            return false;
        }
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
        return true;
    }

    /// For a thread that checks state the lock holder changes often: waits by reading the
    /// owner instead of queueing on the inner lock, so the holder's unlock never has to wake
    /// it. Falls back to lock() after a while.
    void lock_spin() {
        const u32 self = ThisThread();
        if (try_lock()) {
            return;
        }
        // Counted from the first failed try, so that a holder that keeps the lock between its
        // own uses hands it over
        waiters.fetch_add(1, std::memory_order_relaxed);
        bool locked{};
        for (u32 spins = 0; spins < 4096; ++spins) {
            if (owner.load(std::memory_order_relaxed) == 0 && inner.try_lock()) {
                locked = true;
                break;
            }
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#elif defined(__aarch64__) && !defined(_MSC_VER)
            asm volatile("yield");
#endif
        }
        if (!locked) {
            inner.lock();
        }
        waiters.fetch_sub(1, std::memory_order_relaxed);
        owner.store(self, std::memory_order_relaxed);
        depth = 1;
    }

    /// True while another thread waits for the lock
    [[nodiscard]] bool HasWaiters() const noexcept {
        return waiters.load(std::memory_order_relaxed) != 0;
    }

    void unlock() {
        if (--depth != 0) {
            return;
        }
        owner.store(0, std::memory_order_relaxed);
        inner.unlock();
    }

private:
    /// Small nonzero id of the calling thread
    static u32 ThisThread() noexcept {
        static std::atomic<u32> next_id{1};
        thread_local const u32 id = next_id.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    // Only the owner writes its own id, so another thread never reads its own id here
    std::atomic<u32> owner{0};
    std::atomic<u32> waiters{0};
    u32 depth = 0;
    std::shared_mutex inner;
};

} // namespace Common
