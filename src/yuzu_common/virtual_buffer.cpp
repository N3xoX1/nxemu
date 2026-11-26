// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#if defined(__linux__) && (defined(__GLIBC__) || defined(__ANDROID__))
#include <malloc.h>
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#endif
#endif

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/virtual_buffer.h"

namespace Common {

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
