// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>

#include "yuzu_common/common_types.h"

namespace Common {

class RecursiveMutex {
public:
    RecursiveMutex() = default;
    RecursiveMutex(const RecursiveMutex&) = delete;
    RecursiveMutex& operator=(const RecursiveMutex&) = delete;

    void lock() {
        const u64 self = ThisThreadId();
        if (IsOwnedBy(self)) {
            ++depth;
            return;
        }
        plain_lock.lock();
        TakeOwnership(self);
    }

    bool try_lock() {
        const u64 self = ThisThreadId();
        if (IsOwnedBy(self)) {
            ++depth;
            return true;
        }
        if (!plain_lock.try_lock()) {
            return false;
        }
        TakeOwnership(self);
        return true;
    }

    void unlock() {
        if (--depth != 0) {
            return;
        }
        owner.store(NoOwner, std::memory_order_relaxed);
        plain_lock.unlock();
    }

private:
    static constexpr u64 NoOwner = 0;

#ifdef _MSC_VER
    // MSVC's std::shared_mutex is a bare SRW lock, with less bookkeeping than its std::mutex
    using PlainLock = std::shared_mutex;
#else
    using PlainLock = std::mutex;
#endif

    static u64 ThisThreadId() noexcept {
        static std::atomic<u64> next_id{NoOwner + 1};
        thread_local const u64 id = next_id.fetch_add(1, std::memory_order_relaxed);
        return id;
    }

    bool IsOwnedBy(u64 thread_id) const noexcept {
        return owner.load(std::memory_order_relaxed) == thread_id;
    }

    void TakeOwnership(u64 thread_id) noexcept {
        owner.store(thread_id, std::memory_order_relaxed);
        depth = 1;
    }

    std::atomic<u64> owner{NoOwner};
    u32 depth = 0;
    PlainLock plain_lock;
};

} // namespace Common
