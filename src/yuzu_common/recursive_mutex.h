// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <shared_mutex>

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
        inner.lock();
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
    u32 depth = 0;
    std::shared_mutex inner;
};

} // namespace Common
