// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/virtual_buffer.h"

namespace Common {

#ifdef _WIN32
namespace {
// A 39-bit guest address space needs three 1 GiB tables, most of which stay untouched.
// Guard pages keep working-set scans from making unused table pages resident. The first
// real access clears the guard and resumes through this handler.
struct GuardRange {
    std::atomic<uintptr_t> begin{0};
    std::atomic<uintptr_t> end{0};
};
constexpr size_t MinGuardSize = size_t{64} << 20;
std::array<GuardRange, 32> guard_ranges;
std::mutex guard_mutex; // Writers only. The exception handler must remain lock-free.
PVOID guard_handler{};

LONG CALLBACK PageTableGuardHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* const record = info->ExceptionRecord;
    if (record->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || record->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t address = record->ExceptionInformation[1];
    for (const GuardRange& range : guard_ranges) {
        const uintptr_t begin = range.begin.load();
        if (begin && address >= begin && address < range.end.load() && begin == range.begin.load()) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void ReleaseGuardRange(GuardRange& range) {
    range.begin.store(0);
    if (std::none_of(guard_ranges.begin(), guard_ranges.end(),
                     [](const GuardRange& r) { return r.begin.load() != 0; })) {
        RemoveVectoredExceptionHandler(guard_handler);
        guard_handler = nullptr;
    }
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
    const uintptr_t begin = reinterpret_cast<uintptr_t>(data);
    for (const GuardRange& range : guard_ranges) {
        if (range.begin.load(std::memory_order_acquire) == begin) {
            return; // same table resized to the same size: already guarded
        }
    }
    for (GuardRange& range : guard_ranges) {
        if (range.begin.load() == 0) {
            if (!guard_handler) {
                guard_handler = AddVectoredExceptionHandler(1, PageTableGuardHandler);
                if (!guard_handler) {
                    return;
                }
            }
            range.end.store(begin + size);
            range.begin.store(begin);
            DWORD old_protect;
            if (!VirtualProtect(data, size, PAGE_READWRITE | PAGE_GUARD, &old_protect)) {
                ReleaseGuardRange(range);
            }
            return;
        }
    }
}

static void UnguardPages(const void* data) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(data);
    std::scoped_lock lock{guard_mutex};
    for (GuardRange& range : guard_ranges) {
        if (range.begin.load() == begin) {
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

void FreeMemoryPages(void* base, [[maybe_unused]] std::size_t size) noexcept {
    if (!base) {
        return;
    }
#ifdef _WIN32
    if (size >= MinGuardSize) {
        UnguardPages(base);
    }
    ASSERT(VirtualFree(base, 0, MEM_RELEASE));
#else
    ASSERT(munmap(base, size) == 0);
#endif
}

} // namespace Common
