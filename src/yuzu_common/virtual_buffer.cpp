// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#if defined(__linux__) && (defined(__GLIBC__) || defined(__ANDROID__))
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#endif

#include "yuzu_common/common_types.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/virtual_buffer.h"

namespace Common {

#ifdef _WIN32
namespace {
// A 39-bit guest address space needs three 1 GiB tables plus 128 MiB of write tracking, most of
// which stays untouched. Large lazy allocations reserve VA space with zero commit charge and
// commit 64 KiB chunks on first access inside this lock-free VEH. GuardUntouchedPages also keeps
// pre-committed buffers out of the working set via PAGE_GUARD.
struct GuardRange {
    std::atomic<u64> seq{0}; // even = stable, odd = writer updating
    std::atomic<uintptr_t> begin{0};
    std::atomic<uintptr_t> end{0};
    std::atomic<bool> lazy_commit{false};
};
constexpr size_t MinGuardSize = size_t{64} << 20;
constexpr size_t MinLazyCommitSize = size_t{2} << 20;
constexpr size_t LazyCommitChunk = size_t{64} << 10;
std::array<GuardRange, 64> guard_ranges;
std::atomic<size_t> active_range_limit{0};
std::mutex guard_mutex; // Writers only. The exception handler must remain lock-free.
PVOID guard_handler{};

LONG CALLBACK PageTableGuardHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* const record = info->ExceptionRecord;
    const DWORD code = record->ExceptionCode;
    const bool is_av = code == STATUS_ACCESS_VIOLATION;
    const bool is_guard = code == STATUS_GUARD_PAGE_VIOLATION;
    if ((!is_av && !is_guard) || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (is_av) {
        // Only demand-commit read (0) and write (1) faults; never commit on DEP/execute (8).
        const ULONG_PTR access = record->ExceptionInformation[0];
        if (access != 0 && access != 1) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    const uintptr_t address = record->ExceptionInformation[1];
    const size_t limit = active_range_limit.load(std::memory_order_acquire);
    for (size_t i = 0; i < limit; ++i) {
        const GuardRange& range = guard_ranges[i];
        const u64 s1 = range.seq.load(std::memory_order_acquire);
        if ((s1 & 1) != 0) {
            continue;
        }
        const uintptr_t begin = range.begin.load(std::memory_order_relaxed);
        const uintptr_t end = range.end.load(std::memory_order_relaxed);
        const bool lazy = range.lazy_commit.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (range.seq.load(std::memory_order_relaxed) != s1) {
            continue;
        }
        if (begin != 0 && address >= begin && address < end) {
            if (lazy) {
                if (!is_av) {
                    return EXCEPTION_CONTINUE_SEARCH;
                }
                const uintptr_t chunk_begin = address & ~(LazyCommitChunk - 1);
                const size_t chunk_len = std::min(end, chunk_begin + LazyCommitChunk) - chunk_begin;
                if (VirtualAlloc(reinterpret_cast<void*>(chunk_begin), chunk_len, MEM_COMMIT,
                                 PAGE_READWRITE) == nullptr) {
                    return EXCEPTION_CONTINUE_SEARCH;
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (is_guard) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void PublishRange(GuardRange& range, uintptr_t begin, uintptr_t end, bool lazy_commit) {
    const u64 seq = range.seq.load(std::memory_order_relaxed);
    range.seq.store(seq + 1, std::memory_order_release);
    range.begin.store(begin, std::memory_order_relaxed);
    range.end.store(end, std::memory_order_relaxed);
    range.lazy_commit.store(lazy_commit, std::memory_order_relaxed);
    range.seq.store(seq + 2, std::memory_order_release);
}

void UpdateActiveRangeLimit() {
    size_t limit = 0;
    for (size_t i = 0; i < guard_ranges.size(); ++i) {
        if (guard_ranges[i].begin.load(std::memory_order_relaxed) != 0) {
            limit = i + 1;
        }
    }
    active_range_limit.store(limit, std::memory_order_release);
}

void ReleaseGuardRange(GuardRange& range) {
    PublishRange(range, 0, 0, false);
    UpdateActiveRangeLimit();
    if (active_range_limit.load(std::memory_order_relaxed) == 0 && guard_handler != nullptr) {
        RemoveVectoredExceptionHandler(guard_handler);
        guard_handler = nullptr;
    }
}

bool RegisterRangeLocked(uintptr_t begin, size_t size, bool lazy_commit, GuardRange*& out_slot) {
    for (GuardRange& range : guard_ranges) {
        if (range.begin.load(std::memory_order_relaxed) == begin) {
            out_slot = &range;
            return false; // already registered
        }
    }
    for (size_t i = 0; i < guard_ranges.size(); ++i) {
        GuardRange& range = guard_ranges[i];
        if (range.begin.load(std::memory_order_relaxed) == 0) {
            if (!guard_handler) {
                guard_handler = AddVectoredExceptionHandler(1, PageTableGuardHandler);
                if (!guard_handler) {
                    return false;
                }
            }
            PublishRange(range, begin, begin + size, lazy_commit);
            if (i + 1 > active_range_limit.load(std::memory_order_relaxed)) {
                active_range_limit.store(i + 1, std::memory_order_release);
            }
            out_slot = &range;
            return true;
        }
    }
    return false;
}

bool PageTableGuardEnabled() {
    static const bool enabled = [] {
        const char* const flag = std::getenv("NXEMU_PAGE_TABLE_GUARD");
        return flag == nullptr || std::strcmp(flag, "0") != 0;
    }();
    return enabled;
}
} // namespace

void GuardUntouchedPages(void* data, std::size_t size) {
    // Only large tables: small ones are fully used anyway
    if (data == nullptr || size < MinGuardSize || !PageTableGuardEnabled()) {
        return;
    }
    std::scoped_lock lock{guard_mutex};
    GuardRange* slot = nullptr;
    if (!RegisterRangeLocked(reinterpret_cast<uintptr_t>(data), size, false, slot)) {
        return;
    }
    DWORD old_protect;
    if (!VirtualProtect(data, size, PAGE_READWRITE | PAGE_GUARD, &old_protect)) {
        ReleaseGuardRange(*slot);
    }
}

static void UnguardPages(const void* data) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(data);
    std::scoped_lock lock{guard_mutex};
    for (GuardRange& range : guard_ranges) {
        if (range.begin.load(std::memory_order_relaxed) == begin) {
            ReleaseGuardRange(range);
            return;
        }
    }
}
#else
void GuardUntouchedPages(void*, std::size_t) {}
#endif

void* AllocateMemoryPages(std::size_t size) noexcept {
#ifdef _WIN32
    void* base{VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_READWRITE)};
#else
    void* base{mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0)};

    if (base == MAP_FAILED) {
        base = nullptr;
    }
#endif

    ASSERT(base);

    return base;
}

void* AllocateLazyMemoryPages(std::size_t size) noexcept {
#ifdef _WIN32
    if (size >= MinLazyCommitSize && PageTableGuardEnabled()) {
        void* base{VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS)};
        if (base != nullptr) {
            std::scoped_lock lock{guard_mutex};
            GuardRange* slot = nullptr;
            if (RegisterRangeLocked(reinterpret_cast<uintptr_t>(base), size, true, slot)) {
                return base;
            }
            VirtualFree(base, 0, MEM_RELEASE);
        }
    }
    return AllocateMemoryPages(size);
#else
    void* base{mmap(nullptr, size, PROT_READ | PROT_WRITE,
                    MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0)};
    if (base == MAP_FAILED) {
        base = nullptr;
    }
    ASSERT(base);
    return base;
#endif
}

void FreeMemoryPages(void* base, [[maybe_unused]] std::size_t size) noexcept {
    if (!base) {
        return;
    }
#ifdef _WIN32
    if (size >= MinLazyCommitSize) {
        UnguardPages(base);
    }
    ASSERT(VirtualFree(base, 0, MEM_RELEASE));
#else
    ASSERT(munmap(base, size) == 0);
#endif
}

void PurgeHostHeap() noexcept {
#if defined(__ANDROID__)
#if defined(M_PURGE_ALL)
    mallopt(M_PURGE_ALL, 0);
#elif defined(M_PURGE)
    mallopt(M_PURGE, 0);
#endif
#elif defined(__linux__) && defined(__GLIBC__)
    malloc_trim(0);
#elif defined(__APPLE__)
    malloc_zone_pressure_relief(nullptr, 0);
#endif
}

} // namespace Common
