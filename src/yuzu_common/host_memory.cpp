// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32

#include <iterator>
#include <unordered_map>
#include <boost/icl/separate_interval_set.hpp>
#include <windows.h>
#include <psapi.h>
#include "yuzu_common/dynamic_library.h"

#elif defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__) // ^^^ Windows ^^^ vvv POSIX vvv

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <boost/icl/interval_set.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <linux/falloc.h>
#include <sys/random.h>
#endif
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdlib.h>
#include "yuzu_common/nce_direct_map.h"
#endif
#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#include "yuzu_common/scope_exit.h"

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#if defined(__APPLE__) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif

#endif // ^^^ POSIX ^^^

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <vector>

#include "yuzu_common/alignment.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/free_region_manager.h"
#include "yuzu_common/host_memory.h"
#include "yuzu_common/logging/log.h"

namespace Common {

constexpr size_t GuestPageSize = 0x1000;
constexpr size_t HugePageSize = 0x200000;

static size_t GetHostPageSize() {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__)
    static const size_t host_page_size = [] {
        const long value = sysconf(_SC_PAGESIZE);
        ASSERT_MSG(value > 0, "sysconf(_SC_PAGESIZE) failed");
        return static_cast<size_t>(value);
    }();
    return host_page_size;
#else
    return GuestPageSize;
#endif
}

#ifdef _WIN32

// Manually imported for MinGW compatibility
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

using PFN_CreateFileMapping2 = _Ret_maybenull_ HANDLE(WINAPI*)(
    _In_ HANDLE File, _In_opt_ SECURITY_ATTRIBUTES* SecurityAttributes, _In_ ULONG DesiredAccess,
    _In_ ULONG PageProtection, _In_ ULONG AllocationAttributes, _In_ ULONG64 MaximumSize,
    _In_opt_ PCWSTR Name,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_VirtualAlloc2 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress, _In_ SIZE_T Size,
    _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_MapViewOfFile3 = _Ret_maybenull_ PVOID(WINAPI*)(
    _In_ HANDLE FileMapping, _In_opt_ HANDLE Process, _In_opt_ PVOID BaseAddress,
    _In_ ULONG64 Offset, _In_ SIZE_T ViewSize, _In_ ULONG AllocationType, _In_ ULONG PageProtection,
    _Inout_updates_opt_(ParameterCount) MEM_EXTENDED_PARAMETER* ExtendedParameters,
    _In_ ULONG ParameterCount);

using PFN_UnmapViewOfFile2 = BOOL(WINAPI*)(_In_ HANDLE Process, _In_ PVOID BaseAddress,
                                           _In_ ULONG UnmapFlags);

template <typename T>
static void GetFuncAddress(Common::DynamicLibrary& dll, const char* name, T& pfn) {
    if (!dll.GetSymbol(name, &pfn)) {
        LOG_CRITICAL(HW_Memory, "Failed to load {}", name);
        throw std::bad_alloc{};
    }
}

class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}, virtual_size{virtual_size_}, process{GetCurrentProcess()},
          kernelbase_dll("Kernelbase") {
        if (!kernelbase_dll.IsOpen()) {
            LOG_CRITICAL(HW_Memory, "Failed to load Kernelbase.dll");
            throw std::bad_alloc{};
        }
        GetFuncAddress(kernelbase_dll, "CreateFileMapping2", pfn_CreateFileMapping2);
        GetFuncAddress(kernelbase_dll, "VirtualAlloc2", pfn_VirtualAlloc2);
        GetFuncAddress(kernelbase_dll, "MapViewOfFile3", pfn_MapViewOfFile3);
        GetFuncAddress(kernelbase_dll, "UnmapViewOfFile2", pfn_UnmapViewOfFile2);

        if (lazy_commit) {
            const size_t granule_count = (backing_size + CommitGranule - 1) / CommitGranule;
            const size_t word_count = (granule_count + 63) / 64;
            committed_granules = std::make_unique<std::atomic<u64>[]>(word_count);
        }
        // SEC_COMMIT reserves system commit for the *entire* guest memory at startup.
        // SEC_RESERVE only charges pages explicitly committed through the backing view.
        // Keep this opt-in until Windows gameplay and OOM paths have been validated.
        backing_handle =
            pfn_CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_WRITE | FILE_MAP_READ,
                                   PAGE_READWRITE, lazy_commit ? SEC_RESERVE : SEC_COMMIT,
                                   backing_size, nullptr, nullptr, 0);
        if (!backing_handle) {
            LOG_CRITICAL(HW_Memory, "Failed to allocate {} MiB of backing memory",
                         backing_size >> 20);
            throw std::bad_alloc{};
        }
        // Allocate a virtual memory for the backing file map as placeholder
        backing_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, backing_size,
                                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                          PAGE_NOACCESS, nullptr, 0));
        if (!backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} MiB of virtual memory",
                         backing_size >> 20);
            throw std::bad_alloc{};
        }
        // Map backing placeholder
        void* const ret = pfn_MapViewOfFile3(backing_handle, process, backing_base, 0, backing_size,
                                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (ret != backing_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to map {} MiB of virtual memory", backing_size >> 20);
            throw std::bad_alloc{};
        }
        if (lazy_commit) {
            LOG_INFO(HW_Memory, "Windows lazy guest memory commitment enabled ({} MiB reserved)",
                     backing_size >> 20);
        }
        // Allocate virtual address placeholder
        virtual_base = static_cast<u8*>(pfn_VirtualAlloc2(process, nullptr, virtual_size,
                                                          MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                                          PAGE_NOACCESS, nullptr, 0));
        if (!virtual_base) {
            Release();
            LOG_CRITICAL(HW_Memory, "Failed to reserve {} GiB of virtual memory",
                         virtual_size >> 30);
            throw std::bad_alloc{};
        }
    }

    ~Impl() {
        Release();
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
        NoteTrimActivity(length);
        std::unique_lock lock{placeholder_mutex};
        if (!IsNiechePlaceholder(virtual_offset, length)) {
            Split(virtual_offset, length);
        }
        ASSERT(placeholders.find({virtual_offset, virtual_offset + length}) == placeholders.end());
        TrackPlaceholder(virtual_offset, host_offset, length);

        MapView(virtual_offset, host_offset, length);
    }

    void Unmap(size_t virtual_offset, size_t length) {
        NoteTrimActivity(length);
        std::scoped_lock lock{placeholder_mutex};

        // Unmap until there are no more placeholders
        while (UnmapOnePlaceholder(virtual_offset, length)) {
        }
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
        NoteTrimActivity(length);
        DWORD new_flags{};
        if (read && write) {
            new_flags = PAGE_READWRITE;
        } else if (read && !write) {
            new_flags = PAGE_READONLY;
        } else if (!read && !write) {
            new_flags = PAGE_NOACCESS;
        } else {
            UNIMPLEMENTED_MSG("Protection flag combination read={} write={}", read, write);
        }
        const size_t virtual_end = virtual_offset + length;

        std::scoped_lock lock{placeholder_mutex};
        auto [it, end] = placeholders.equal_range({virtual_offset, virtual_end});
        while (it != end) {
            const size_t offset = std::max(it->lower(), virtual_offset);
            const size_t protect_length = std::min(it->upper(), virtual_end) - offset;
            DWORD old_flags{};
            if (!VirtualProtect(virtual_base + offset, protect_length, new_flags, &old_flags)) {
                LOG_CRITICAL(HW_Memory, "Failed to change virtual memory protect rules");
            }
            ++it;
        }
    }

    bool ClearBackingRegion(size_t physical_offset, size_t length) {
        // TODO: This does not seem to be possible on Windows.
        return false;
    }

    bool CommitBackingRegion(size_t physical_offset, size_t length) {
        if (!lazy_commit || length == 0) {
            return true;
        }
        ASSERT(physical_offset <= backing_size && length <= backing_size - physical_offset);

        const size_t first = physical_offset / CommitGranule;
        const size_t last = (physical_offset + length - 1) / CommitGranule + 1;

        // Most fastmem maps and kernel opens revisit already committed pages.
        // Avoid serializing them on commit_mutex; a released bitmap bit means
        // the OS commit succeeded and is visible to all aliases.
        bool already_committed = true;
        for (size_t i = first; i < last; ++i) {
            if ((committed_granules[i / 64].load(std::memory_order_acquire) &
                 (u64{1} << (i % 64))) == 0) {
                already_committed = false;
                break;
            }
        }
        if (already_committed) {
            return true;
        }

        std::scoped_lock lock{commit_mutex};

        size_t index = first;
        while (index < last) {
            if (committed_granules[index / 64].load(std::memory_order_relaxed) &
                (u64{1} << (index % 64))) {
                ++index;
                continue;
            }
            const size_t begin = index;
            do {
                ++index;
            } while (index < last &&
                     !(committed_granules[index / 64].load(std::memory_order_relaxed) &
                       (u64{1} << (index % 64))));

            const size_t offset = begin * CommitGranule;
            const size_t commit_length = std::min(backing_size, index * CommitGranule) - offset;
            void* const address = backing_base + offset;
            if (VirtualAlloc(address, commit_length, MEM_COMMIT, PAGE_READWRITE) != address) {
                LOG_CRITICAL(HW_Memory,
                             "Failed to commit {} KiB of guest memory at offset {:#x} "
                             "(Windows error {})",
                             commit_length >> 10, offset, GetLastError());
                return false;
            }
            for (size_t committed = begin; committed < index; ++committed) {
                committed_granules[committed / 64].fetch_or(u64{1} << (committed % 64),
                                                             std::memory_order_release);
            }
        }
        return true;
    }

    /// Find the physical offset for a resident guest alias. The lock is not held
    /// while reading the backing page or trimming a view from the working set.
    std::optional<std::pair<size_t, size_t>> BackingRange(size_t virtual_offset) {
        std::scoped_lock lock{placeholder_mutex};
        const auto it = placeholders.find(virtual_offset);
        if (it == placeholders.end() || virtual_offset < it->lower() ||
            virtual_offset >= it->upper()) {
            return std::nullopt;
        }
        const auto backing = placeholder_host_pointers.find(it->lower());
        if (backing == placeholder_host_pointers.end()) {
            return std::nullopt;
        }
        return std::pair{backing->second + virtual_offset - it->lower(), it->upper()};
    }

    /// With lazy commitment a stale alias lookup can name a reserved, never
    /// committed backing page; reading it would raise an access violation.
    [[nodiscard]] bool IsBackingCommitted(size_t physical_offset) const noexcept {
        if (!lazy_commit) {
            return true;
        }
        const size_t index = physical_offset / CommitGranule;
        return (committed_granules[index / 64].load(std::memory_order_acquire) &
                (u64{1} << (index % 64))) != 0;
    }

    [[nodiscard]] u64 MappedBytes() const noexcept {
        return mapped_bytes.load(std::memory_order_relaxed);
    }

    void SetTrimActivityCallback(std::function<void()> callback) {
        std::scoped_lock lock{trim_callback_mutex};
        trim_activity_callback = std::move(callback);
    }

    void NoteTrimActivity(size_t length) {
        if (!track_trim_activity || length == 0) {
            return;
        }
        mapped_bytes.fetch_add(length, std::memory_order_relaxed);
        constexpr u64 NotifyThreshold = u64{64} << 20;
        if (trim_pending_bytes.fetch_add(length, std::memory_order_relaxed) + length >=
            NotifyThreshold) {
            if (trim_pending_bytes.exchange(0, std::memory_order_relaxed) >= NotifyThreshold) {
                std::scoped_lock lock{trim_callback_mutex};
                if (trim_activity_callback) {
                    trim_activity_callback();
                }
            }
        }
    }

    void EnableDirectMappedAddress() {
        // TODO
        UNREACHABLE();
    }

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes

    u8* backing_base{};
    u8* virtual_base{};

private:
    static constexpr size_t CommitGranule = 64 * 1024;

    static bool ZeroTrimEnabled() {
        const char* const flag = std::getenv("NXEMU_ZERO_TRIM");
        return flag == nullptr || std::strcmp(flag, "0") != 0;
    }

    static bool LazyCommitEnabled() {
        const char* const flag = std::getenv("NXEMU_WIN_LAZY_GUEST_COMMIT");
        return flag && std::strcmp(flag, "1") == 0;
    }

    /// Release all resources in the object
    void Release() {
        if (!placeholders.empty()) {
            for (const auto& placeholder : placeholders) {
                if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder.lower(),
                                          MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_CRITICAL(HW_Memory, "Failed to unmap virtual memory placeholder");
                }
            }
            Coalesce(0, virtual_size);
        }
        if (virtual_base) {
            if (!VirtualFree(virtual_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free virtual memory");
            }
        }
        if (backing_base) {
            if (!pfn_UnmapViewOfFile2(process, backing_base, MEM_PRESERVE_PLACEHOLDER)) {
                LOG_CRITICAL(HW_Memory, "Failed to unmap backing memory placeholder");
            }
            if (!VirtualFreeEx(process, backing_base, 0, MEM_RELEASE)) {
                LOG_CRITICAL(HW_Memory, "Failed to free backing memory");
            }
        }
        if (!CloseHandle(backing_handle)) {
            LOG_CRITICAL(HW_Memory, "Failed to free backing memory file handle");
        }
    }

    /// Unmap one placeholder in the given range (partial unmaps are supported)
    /// Return true when there are no more placeholders to unmap
    bool UnmapOnePlaceholder(size_t virtual_offset, size_t length) {
        const auto it = placeholders.find({virtual_offset, virtual_offset + length});
        const auto begin = placeholders.begin();
        const auto end = placeholders.end();
        if (it == end) {
            return false;
        }
        const size_t placeholder_begin = it->lower();
        const size_t placeholder_end = it->upper();
        const size_t unmap_begin = std::max(virtual_offset, placeholder_begin);
        const size_t unmap_end = std::min(virtual_offset + length, placeholder_end);
        ASSERT(unmap_begin >= placeholder_begin && unmap_begin < placeholder_end);
        ASSERT(unmap_end <= placeholder_end && unmap_end > placeholder_begin);

        const auto host_pointer_it = placeholder_host_pointers.find(placeholder_begin);
        ASSERT(host_pointer_it != placeholder_host_pointers.end());
        const size_t host_offset = host_pointer_it->second;

        const bool split_left = unmap_begin > placeholder_begin;
        const bool split_right = unmap_end < placeholder_end;

        if (!pfn_UnmapViewOfFile2(process, virtual_base + placeholder_begin,
                                  MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to unmap placeholder");
        }
        // If we have to remap memory regions due to partial unmaps, we are in a data race as
        // Windows doesn't support remapping memory without unmapping first. Avoid adding any extra
        // logic within the panic region described below.

        // Panic region, we are in a data race right now
        if (split_left || split_right) {
            Split(unmap_begin, unmap_end - unmap_begin);
        }
        if (split_left) {
            MapView(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            MapView(unmap_end, host_offset + unmap_end - placeholder_begin,
                    placeholder_end - unmap_end);
        }
        // End panic region

        size_t coalesce_begin = unmap_begin;
        if (!split_left) {
            // Try to coalesce pages to the left
            coalesce_begin = it == begin ? 0 : std::prev(it)->upper();
            if (coalesce_begin != placeholder_begin) {
                Coalesce(coalesce_begin, unmap_end - coalesce_begin);
            }
        }
        if (!split_right) {
            // Try to coalesce pages to the right
            const auto next = std::next(it);
            const size_t next_begin = next == end ? virtual_size : next->lower();
            if (placeholder_end != next_begin) {
                // We can coalesce to the right
                Coalesce(coalesce_begin, next_begin - coalesce_begin);
            }
        }
        // Remove and reinsert placeholder trackers
        UntrackPlaceholder(it);
        if (split_left) {
            TrackPlaceholder(placeholder_begin, host_offset, unmap_begin - placeholder_begin);
        }
        if (split_right) {
            TrackPlaceholder(unmap_end, host_offset + unmap_end - placeholder_begin,
                             placeholder_end - unmap_end);
        }
        return true;
    }

    void MapView(size_t virtual_offset, size_t host_offset, size_t length) {
        if (!pfn_MapViewOfFile3(backing_handle, process, virtual_base + virtual_offset, host_offset,
                                length, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
            LOG_CRITICAL(HW_Memory, "Failed to map placeholder");
        }
    }

    void Split(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_CRITICAL(HW_Memory, "Failed to split placeholder");
        }
    }

    void Coalesce(size_t virtual_offset, size_t length) {
        if (!VirtualFreeEx(process, reinterpret_cast<LPVOID>(virtual_base + virtual_offset), length,
                           MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            LOG_CRITICAL(HW_Memory, "Failed to coalesce placeholders");
        }
    }

    void TrackPlaceholder(size_t virtual_offset, size_t host_offset, size_t length) {
        placeholders.insert({virtual_offset, virtual_offset + length});
        placeholder_host_pointers.emplace(virtual_offset, host_offset);
    }

    void UntrackPlaceholder(boost::icl::separate_interval_set<size_t>::iterator it) {
        placeholder_host_pointers.erase(it->lower());
        placeholders.erase(it);
    }

    /// Return true when a given memory region is a "nieche" and the placeholders don't have to be
    /// split.
    bool IsNiechePlaceholder(size_t virtual_offset, size_t length) const {
        const auto it = placeholders.upper_bound({virtual_offset, virtual_offset + length});
        if (it != placeholders.end() && it->lower() == virtual_offset + length) {
            return it == placeholders.begin() ? virtual_offset == 0
                                              : std::prev(it)->upper() == virtual_offset;
        }
        return false;
    }

    HANDLE process{};        ///< Current process handle
    HANDLE backing_handle{}; ///< File based backing memory

    const bool lazy_commit{LazyCommitEnabled()};
    const bool track_trim_activity{ZeroTrimEnabled()};
    std::atomic<u64> mapped_bytes{0};
    std::atomic<u64> trim_pending_bytes{0};
    std::mutex trim_callback_mutex;
    std::function<void()> trim_activity_callback;
    std::mutex commit_mutex;
    std::unique_ptr<std::atomic<u64>[]> committed_granules;

    DynamicLibrary kernelbase_dll;
    PFN_CreateFileMapping2 pfn_CreateFileMapping2{};
    PFN_VirtualAlloc2 pfn_VirtualAlloc2{};
    PFN_MapViewOfFile3 pfn_MapViewOfFile3{};
    PFN_UnmapViewOfFile2 pfn_UnmapViewOfFile2{};

    std::mutex placeholder_mutex;                                 ///< Mutex for placeholders
    boost::icl::separate_interval_set<size_t> placeholders;       ///< Mapped placeholders
    std::unordered_map<size_t, size_t> placeholder_host_pointers; ///< Placeholder backing offset
};

#elif defined(__linux__) || defined(__FreeBSD__) || defined(__APPLE__) // ^^^ Windows ^^^ vvv POSIX vvv

#if defined(__APPLE__)

static void* ChooseVirtualBase(size_t virtual_size) {
    return mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

#elif defined(_M_ARM64) || defined(ARCHITECTURE_arm64)

static void* ChooseVirtualBase(size_t virtual_size) {
    constexpr uintptr_t Map39BitSize = (1ULL << 39);
    constexpr uintptr_t Map36BitSize = (1ULL << 36);

    // This is not a cryptographic application, we just want something random.
    std::mt19937_64 rng;

    // We want to ensure we are allocating at an address aligned to the L2 block size.
    // For Qualcomm devices, we must also allocate memory above 36 bits.
    const size_t lower = Map36BitSize / HugePageSize;
    const size_t upper = (Map39BitSize - virtual_size) / HugePageSize;
    const size_t range = upper - lower;

    // Try up to 64 times to allocate memory at random addresses in the range.
    for (int i = 0; i < 64; i++) {
        // Calculate a possible location.
        uintptr_t hint_address = ((rng() % range) + lower) * HugePageSize;

        // Try to map.
        // Note: we may be able to take advantage of MAP_FIXED_NOREPLACE here.
        void* map_pointer =
            mmap(reinterpret_cast<void*>(hint_address), virtual_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

        // If we successfully mapped, we're done.
        if (reinterpret_cast<uintptr_t>(map_pointer) == hint_address) {
            return map_pointer;
        }

        // Unmap if necessary, and try again.
        if (map_pointer != MAP_FAILED) {
            munmap(map_pointer, virtual_size);
        }
    }

    return MAP_FAILED;
}

#else

static void* ChooseVirtualBase(size_t virtual_size) {
#if defined(__FreeBSD__)
    void* virtual_base =
        mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_ALIGNED_SUPER, -1, 0);

    if (virtual_base != MAP_FAILED) {
        return virtual_base;
    }
#endif

    return mmap(nullptr, virtual_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
}

#endif

#if defined(__ANDROID__) && __ANDROID_API__ < 30
static int memfd_create(const char* name, unsigned int flags) {
    return syscall(__NR_memfd_create, name, flags);
}
#endif
class HostMemory::Impl {
public:
    explicit Impl(size_t backing_size_, size_t virtual_size_)
        : backing_size{backing_size_}, virtual_size{virtual_size_} {
        bool good = false;
        SCOPE_EXIT {
            if (!good) {
                Release();
            }
        };

#if !defined(__APPLE__)
        if (host_page_size != GuestPageSize) {
            LOG_CRITICAL(HW_Memory, "page size {:#x} is incompatible with 4K paging",
                         host_page_size);
            throw std::bad_alloc{};
        }
#endif

        // Backing memory initialization
#if defined(__FreeBSD__) && __FreeBSD__ < 13
        // XXX Drop after FreeBSD 12.* reaches EOL on 2024-06-30
        fd = shm_open(SHM_ANON, O_RDWR, 0600);
#elif defined(__APPLE__)
        // A Mach named memory entry rather than a shm object: code mapped from a shm object is
        // not executable, and native code execution maps guest code from the backing.
        memory_object_size_t entry_size = backing_size;
        kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &entry_size, 0,
                                                     MAP_MEM_NAMED_CREATE | VM_PROT_ALL,
                                                     &backing_entry, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            LOG_CRITICAL(HW_Memory, "mach_make_memory_entry_64 failed: {}", kr);
            throw std::bad_alloc{};
        }
        mach_vm_address_t backing_address = 0;
        kr = mach_vm_map(mach_task_self(), &backing_address, backing_size, 0, VM_FLAGS_ANYWHERE,
                         backing_entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL,
                         VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) {
            LOG_CRITICAL(HW_Memory, "mach_vm_map of the backing failed: {}", kr);
            throw std::bad_alloc{};
        }
        backing_base = reinterpret_cast<u8*>(backing_address);
#else
        fd = memfd_create("HostMemory", 0);
#endif
#if !defined(__APPLE__)
        if (fd < 0) {
            LOG_CRITICAL(HW_Memory, "Failed to create HostMemory backing file: {}",
                         strerror(errno));
            throw std::bad_alloc{};
        }

        // Defined to extend the file with zeros
        int ret = ftruncate(fd, backing_size);
        if (ret != 0) {
            LOG_CRITICAL(HW_Memory, "ftruncate failed with {}, are you out-of-memory?",
                         strerror(errno));
            throw std::bad_alloc{};
        }

        backing_base = static_cast<u8*>(
            mmap(nullptr, backing_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (backing_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }
#endif

#if defined(__APPLE__)
        if (host_page_size != GuestPageSize) {
            LOG_INFO(HW_Memory,
                     "Fastmem disabled: host page size {:#x} does not match guest page size {:#x}; "
                     "using the page-table memory path",
                     host_page_size, GuestPageSize);
            virtual_base = nullptr;
            good = true;
            return;
        }
#endif

        // Virtual memory initialization. This path is only used when the host can represent
        // the guest's 4 KiB mappings directly.
        virtual_base = virtual_map_base = static_cast<u8*>(ChooseVirtualBase(virtual_size));
        if (virtual_base == MAP_FAILED) {
            LOG_CRITICAL(HW_Memory, "mmap failed: {}", strerror(errno));
            throw std::bad_alloc{};
        }
#if defined(__linux__)
        madvise(virtual_base, virtual_size, MADV_HUGEPAGE);
#endif

        free_manager.SetAddressSpace(virtual_base, virtual_size);
        good = true;
    }

    ~Impl() {
        Release();
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perms) {
#if defined(__APPLE__)
        if (nce_map) {
            nce_map->Map(virtual_offset, host_offset, length, static_cast<u32>(perms));
            return;
        }
#endif
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);
        if (length == 0) {
            return;
        }

        // We are removing a placeholder.
        free_manager.AllocateBlock(virtual_base + virtual_offset, length);

        // Deduce mapping protection flags.
        int flags = PROT_NONE;
        if (True(perms & MemoryPermission::Read)) {
            flags |= PROT_READ;
        }
        if (True(perms & MemoryPermission::Write)) {
            flags |= PROT_WRITE;
        }
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64)
        if (True(perms & MemoryPermission::Execute)) {
            flags |= PROT_EXEC;
        }
#endif

        void* ret = mmap(virtual_base + virtual_offset, length, flags, MAP_SHARED | MAP_FIXED, fd,
                         host_offset);
        ASSERT_MSG(ret != MAP_FAILED, "mmap failed: {}", strerror(errno));
    }

    void Unmap(size_t virtual_offset, size_t length) {
#if defined(__APPLE__)
        if (nce_map) {
            nce_map->Unmap(virtual_offset, length);
            return;
        }
#endif
        // The method name is wrong. We're still talking about the virtual range.
        // We don't want to unmap, we want to reserve this memory.

        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);
        if (length == 0) {
            return;
        }

        // Merge with any adjacent placeholder mappings.
        auto [merged_pointer, merged_size] =
            free_manager.FreeBlock(virtual_base + virtual_offset, length);

        void* ret = mmap(merged_pointer, merged_size, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        ASSERT_MSG(ret != MAP_FAILED, "mmap failed: {}", strerror(errno));
    }

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {
#if defined(__APPLE__)
        if (nce_map) {
            nce_map->Protect(virtual_offset, length,
                             (read ? NceDirectMap::PermRead : 0) |
                                 (write ? NceDirectMap::PermWrite : 0) |
                                 (execute ? NceDirectMap::PermExecute : 0));
            return;
        }
#endif
        // Intersect the range with our address space.
        AdjustMap(&virtual_offset, &length);
        if (length == 0) {
            return;
        }

        int flags = PROT_NONE;
        if (read) {
            flags |= PROT_READ;
        }
        if (write) {
            flags |= PROT_WRITE;
        }
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64)
        if (execute) {
            flags |= PROT_EXEC;
        }
#endif

        int ret = mprotect(virtual_base + virtual_offset, length, flags);
        ASSERT_MSG(ret == 0, "mprotect failed: {}", strerror(errno));
    }

    bool ClearBackingRegion(size_t physical_offset, size_t length) {
#ifdef __linux__
        // madvise rounds the length up to a host page. Let the caller memset partial pages
        // so an adjacent allocation keeps its bytes.
        if (!Common::IsAligned(physical_offset, host_page_size) ||
            !Common::IsAligned(length, host_page_size)) {
            return false;
        }
        // Set MADV_REMOVE on backing map to destroy it instantly.
        // This also deletes the area from the backing file.
        enum class HolePunchMode : u8 { MadviseRemove, FallocatePunch, Unsupported };
        static std::atomic<u8> hole_punch_mode{static_cast<u8>(HolePunchMode::MadviseRemove)};
        const auto is_permanent_unsupported = [](int err) {
            return err == EOPNOTSUPP || err == EINVAL || err == ENOSYS || err == EPERM ||
                   err == EACCES;
        };
        const auto mode = static_cast<HolePunchMode>(hole_punch_mode.load(std::memory_order_relaxed));
        if (mode == HolePunchMode::Unsupported) {
            return false;
        }
        if (mode == HolePunchMode::MadviseRemove &&
            madvise(backing_base + physical_offset, length, MADV_REMOVE) == 0) {
            return true;
        }
        // Some Android kernels reject MADV_REMOVE on the memfd. Punching the hole in the file
        // frees the pages the same way; the memset fallback would keep them resident as zeros.
        if (fd >= 0) {
            int ret;
            do {
                ret = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                                static_cast<off_t>(physical_offset), static_cast<off_t>(length));
            } while (ret != 0 && errno == EINTR);
            if (ret == 0) {
                if (hole_punch_mode.exchange(static_cast<u8>(HolePunchMode::FallocatePunch),
                                             std::memory_order_relaxed) ==
                    static_cast<u8>(HolePunchMode::MadviseRemove)) {
                    LOG_WARNING(HW_Memory,
                                "MADV_REMOVE unsupported; punching holes in the backing instead");
                }
                return true;
            }
        }
        // Let HostMemory zero the region instead of terminating the emulator.
        const int error = errno;
        if (fd < 0 || is_permanent_unsupported(error)) {
            hole_punch_mode.store(static_cast<u8>(HolePunchMode::Unsupported),
                                  std::memory_order_relaxed);
        }
        static std::once_flag warning_once;
        std::call_once(warning_once, [error] {
            LOG_WARNING(HW_Memory, "MADV_REMOVE and hole punching unsupported ({}); using memset fallback",
                        strerror(error));
        });
        return false;
#elif defined(__APPLE__)
        // The backing is a Mach named memory entry, which has no hole punching. Zero the region,
        // then mark its whole host pages reusable: the kernel takes them back without writing
        // them out, and a page it took reads as zero again. Either way the region reads as zero.
        u8* const data = backing_base + physical_offset;
        std::memset(data, 0, length);
        MarkReusable(data, length);
        return true;
#else
        return false;
#endif
    }

#if defined(__APPLE__)
    /// Hands the whole host pages inside [data, data + length) back to the kernel as reusable.
    /// Their bytes stay valid until the kernel takes them, after which they read as zero.
    void MarkReusable(u8* data, size_t length) const {
        const uintptr_t begin = Common::AlignUp(reinterpret_cast<uintptr_t>(data), host_page_size);
        const uintptr_t end = Common::AlignDown(reinterpret_cast<uintptr_t>(data) + length,
                                                host_page_size);
        if (begin < end) {
            madvise(reinterpret_cast<void*>(begin), end - begin, MADV_FREE_REUSABLE);
        }
    }
#endif

    bool CommitBackingRegion(size_t physical_offset, size_t length) {
        return true;
    }

    void EnableDirectMappedAddress() {
        virtual_base = nullptr;
    }

#if defined(__APPLE__)
    /// Native code execution on 16 KiB pages: guest memory is mapped at the guest addresses by
    /// an NceDirectMap instead of the 4 KiB fastmem arena. Returns the lowest guest address.
    u8* EnableNceDirectMap() {
        // Room for the code, heap (8 GiB), stack (2 GiB) and kernel map regions, plus free
        // address space for the guest ASLR allocations.
        constexpr u64 MinLowWindowSize = 24ULL << 30;
        auto map = std::make_unique<NceDirectMap>(backing_entry, backing_base);
        if (!map->Reserve(MinLowWindowSize)) {
            LOG_CRITICAL(HW_Memory, "NCE: unable to reserve the guest address windows");
            return nullptr;
        }
        nce_map = std::move(map);
        return reinterpret_cast<u8*>(nce_map->Windows().front().start);
    }

    std::unique_ptr<NceDirectMap> nce_map;
    mach_port_t backing_entry{MACH_PORT_NULL};
#endif

    const size_t backing_size; ///< Size of the backing memory in bytes
    const size_t virtual_size; ///< Size of the virtual address placeholder in bytes
    const size_t host_page_size{GetHostPageSize()};

    u8* backing_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_base{reinterpret_cast<u8*>(MAP_FAILED)};
    u8* virtual_map_base{reinterpret_cast<u8*>(MAP_FAILED)};

private:
    /// Release all resources in the object
    void Release() {
        if (virtual_map_base != MAP_FAILED) {
            int ret = munmap(virtual_map_base, virtual_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (backing_base != MAP_FAILED) {
            int ret = munmap(backing_base, backing_size);
            ASSERT_MSG(ret == 0, "munmap failed: {}", strerror(errno));
        }

        if (fd != -1) {
            int ret = close(fd);
            ASSERT_MSG(ret == 0, "close failed: {}", strerror(errno));
        }
#if defined(__APPLE__)
        nce_map.reset();
        if (backing_entry != MACH_PORT_NULL) {
            mach_port_deallocate(mach_task_self(), backing_entry);
        }
#endif
    }

    void AdjustMap(size_t* virtual_offset, size_t* length) {
        if (virtual_base != nullptr) {
            return;
        }

        // If we are direct mapped, we want to make sure we are operating on a region
        // that is in range of our virtual mapping.
        size_t intended_start = *virtual_offset;
        size_t intended_end = intended_start + *length;
        size_t address_space_start = reinterpret_cast<size_t>(virtual_map_base);
        size_t address_space_end = address_space_start + virtual_size;

        if (address_space_start > intended_end || intended_start > address_space_end) {
            *virtual_offset = 0;
            *length = 0;
        } else {
            *virtual_offset = std::max(intended_start, address_space_start);
            *length = std::min(intended_end, address_space_end) - *virtual_offset;
        }
    }

    int fd{-1}; // Shared backing file descriptor; -1 means no file.
    FreeRegionManager free_manager{};
};

#else // ^^^ POSIX ^^^ vvv Generic vvv

class HostMemory::Impl {
public:
    explicit Impl(size_t /*backing_size */, size_t /* virtual_size */) {
        // This is just a place holder.
        // Please implement fastmem in a proper way on your platform.
        throw std::bad_alloc{};
    }

    void Map(size_t virtual_offset, size_t host_offset, size_t length, MemoryPermission perm) {}

    void Unmap(size_t virtual_offset, size_t length) {}

    void Protect(size_t virtual_offset, size_t length, bool read, bool write, bool execute) {}

    bool ClearBackingRegion(size_t physical_offset, size_t length) {
        return false;
    }

    bool CommitBackingRegion(size_t physical_offset, size_t length) {
        return true;
    }

    void EnableDirectMappedAddress() {}

    u8* backing_base{nullptr};
    u8* virtual_base{nullptr};
};

#endif // ^^^ Generic ^^^

HostMemory::HostMemory(size_t backing_size_, size_t virtual_size_)
    : backing_size(backing_size_), virtual_size(virtual_size_) {
    try {
        // Try to allocate a fastmem arena.
        // The implementation will fail with std::bad_alloc on errors.
        impl =
            std::make_unique<HostMemory::Impl>(AlignUp(backing_size, GetHostPageSize()),
                                               AlignUp(virtual_size, GetHostPageSize()) +
                                                   HugePageSize);
        backing_base = impl->backing_base;
        virtual_base = impl->virtual_base;

        if (virtual_base) {
            // Ensure the virtual base is aligned to the L2 block size.
            virtual_base = reinterpret_cast<u8*>(
                Common::AlignUp(reinterpret_cast<uintptr_t>(virtual_base), HugePageSize));
            virtual_base_offset = virtual_base - impl->virtual_base;
        }

    } catch (const std::bad_alloc&) {
        LOG_CRITICAL(HW_Memory,
                     "Fastmem unavailable, falling back to VirtualBuffer for memory allocation");
        fallback_buffer = std::make_unique<Common::VirtualBuffer<u8>>(backing_size);
        backing_base = fallback_buffer->data();
        virtual_base = nullptr;
    }
    num_granules = (backing_size + (size_t{1} << GranuleShift) - 1) >> GranuleShift;
    granules = std::make_unique<std::atomic<u8>[]>(num_granules);
}

HostMemory::~HostMemory() = default;

HostMemory::HostMemory(HostMemory&&) noexcept = default;

HostMemory& HostMemory::operator=(HostMemory&&) noexcept = default;

void HostMemory::Map(size_t virtual_offset, size_t host_offset, size_t length,
                     MemoryPermission perms, bool separate_heap) {
    ASSERT(virtual_offset % GuestPageSize == 0);
    ASSERT(host_offset % GuestPageSize == 0);
    ASSERT(length % GuestPageSize == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    ASSERT(host_offset + length <= backing_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
#ifdef _WIN32
    // An alias may be touched immediately after it becomes visible, even when its
    // physical pages were not obtained via a regular kernel heap allocation.
    CommitBackingRegion(host_offset, length);
#endif
    impl->Map(virtual_offset + virtual_base_offset, host_offset, length, perms);
}

void HostMemory::Unmap(size_t virtual_offset, size_t length, bool separate_heap) {
    ASSERT(virtual_offset % GuestPageSize == 0);
    ASSERT(length % GuestPageSize == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    impl->Unmap(virtual_offset + virtual_base_offset, length);
}

void HostMemory::Protect(size_t virtual_offset, size_t length, MemoryPermission perm) {
    ASSERT(virtual_offset % GuestPageSize == 0);
    ASSERT(length % GuestPageSize == 0);
    ASSERT(virtual_offset + length <= virtual_size);
    if (length == 0 || !virtual_base || !impl) {
        return;
    }
    const bool read = True(perm & MemoryPermission::Read);
    const bool write = True(perm & MemoryPermission::Write);
    const bool execute = True(perm & MemoryPermission::Execute);
    impl->Protect(virtual_offset + virtual_base_offset, length, read, write, execute);
}

void HostMemory::ClearBackingRegion(size_t physical_offset, size_t length, u32 fill_value) {
#ifdef _WIN32
    CommitBackingRegion(physical_offset, length);
#endif
    if (length == 0) {
        return;
    }
    MarkBackingUsed(physical_offset, length);
    const auto clear = [this, fill_value](size_t offset, size_t size) {
        if (!impl || fill_value != 0 || !impl->ClearBackingRegion(offset, size)) {
            std::memset(backing_base + offset, fill_value, size);
        }
    };
    if (fill_value != 0) {
        clear(physical_offset, length);
        MarkBackingDirty(physical_offset, length);
        return;
    }
    // Clear only the granules that may hold data; the rest is still zero and untouched, and a
    // memset would make the host commit it (and spend the time) for nothing. A granule
    // cleared in full is zero again.
    constexpr size_t GranuleSize = size_t{1} << GranuleShift;
    const size_t end = physical_offset + length;
    size_t span_begin = end;
    for (size_t offset = physical_offset; offset < end;) {
        const size_t index = offset >> GranuleShift;
        const size_t granule_end = (index + 1) << GranuleShift;
        const size_t clear_end = std::min(granule_end, end);
        const bool dirty =
            (granules[index].load(std::memory_order_acquire) & GranuleDirty) != 0;
        if (dirty) {
            if (span_begin == end) {
                span_begin = offset;
            }
            if (offset == granule_end - GranuleSize && clear_end == granule_end) {
                // Cleared below, before anyone can see this granule allocated.
                granules[index].fetch_and(static_cast<u8>(~GranuleDirty),
                                          std::memory_order_release);
            }
        } else if (span_begin != end) {
            clear(span_begin, offset - span_begin);
            span_begin = end;
        }
        offset = clear_end;
    }
    if (span_begin != end) {
        clear(span_begin, end - span_begin);
    }
}

void HostMemory::MarkBackingUsed(size_t physical_offset, size_t length) {
    if (length == 0) {
        return;
    }
    const size_t last = std::min((physical_offset + length - 1) >> GranuleShift, num_granules - 1);
    for (size_t index = physical_offset >> GranuleShift; index <= last; ++index) {
        if ((granules[index].load(std::memory_order_relaxed) & GranuleUsed) == 0) {
            granules[index].fetch_or(GranuleUsed, std::memory_order_release);
        }
    }
}

void HostMemory::MarkBackingDirty(size_t physical_offset, size_t length) {
    if (length == 0) {
        return;
    }
    const size_t last = std::min((physical_offset + length - 1) >> GranuleShift, num_granules - 1);
    for (size_t index = physical_offset >> GranuleShift; index <= last; ++index) {
        if ((granules[index].load(std::memory_order_relaxed) & GranuleDirty) == 0) {
            granules[index].fetch_or(GranuleUsed | GranuleDirty, std::memory_order_release);
        }
    }
#ifdef _WIN32
    if (impl) {
        impl->NoteTrimActivity(length);
    }
#endif
}

bool HostMemory::IsBackingDirty(size_t physical_offset) const {
    const size_t index = physical_offset >> GranuleShift;
    return index < num_granules &&
           (granules[index].load(std::memory_order_acquire) & GranuleDirty) != 0;
}

bool HostMemory::ReleaseFreeBacking(size_t physical_offset) {
    return ReleaseFreeBackingRange(physical_offset, size_t{1} << GranuleShift);
}

bool HostMemory::ReleaseFreeBackingRange(size_t physical_offset, size_t length) {
    constexpr size_t GranuleSize = size_t{1} << GranuleShift;
    if (!impl || length == 0 || physical_offset % GranuleSize != 0 ||
        length % GranuleSize != 0 || physical_offset > backing_size ||
        backing_size - physical_offset < length) {
        return false;
    }
#ifdef _WIN32
    // Windows frees an all-zero page that leaves the working set without writing it to the
    // pagefile, so zeroing and trimming returns the RAM (not the commit).
    std::memset(backing_base + physical_offset, 0, length);
    VirtualUnlock(backing_base + physical_offset, length);
#elif defined(__linux__) || defined(__APPLE__)
    if (!impl->ClearBackingRegion(physical_offset, length)) {
        return false;
    }
#else
    return false;
#endif
    const size_t first = physical_offset >> GranuleShift;
    const size_t count = length >> GranuleShift;
    for (size_t i = 0; i < count; ++i) {
        granules[first + i].fetch_and(static_cast<u8>(~GranuleDirty),
                                      std::memory_order_release);
    }
    return true;
}

void HostMemory::ForgetBackingContents() {
    for (size_t index = 0; index < num_granules; ++index) {
        if ((granules[index].load(std::memory_order_relaxed) & GranuleUsed) != 0) {
            granules[index].fetch_or(GranuleDirty, std::memory_order_release);
        }
    }
}

void HostMemory::CommitBackingRegion(size_t physical_offset, size_t length) {
    ASSERT(physical_offset <= backing_size && length <= backing_size - physical_offset);
    if (impl && !impl->CommitBackingRegion(physical_offset, length)) {
        // An access violation would otherwise occur on the first touch of a reserved page.
        throw std::bad_alloc{};
    }
}

std::pair<size_t, size_t> HostMemory::TrimZeroPages() const {
#ifdef _WIN32
    if (backing_base == nullptr || impl == nullptr) {
        return {0, 0};
    }

    // Query resident pages only: scanning the entire 4-12 GiB reservation would
    // page cold memory back in and negate the benefit. The working-set API can
    // report a larger required buffer when the process grows during the query.
    std::vector<ULONG_PTR> entries(size_t{1} << 17);
    bool queried = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (K32QueryWorkingSet(GetCurrentProcess(), entries.data(),
                               static_cast<DWORD>(entries.size() * sizeof(ULONG_PTR)))) {
            queried = true;
            break;
        }
        if (GetLastError() != ERROR_BAD_LENGTH || entries.size() >= (size_t{1} << 23)) {
            break;
        }
        entries.resize(std::min(entries.size() * 2, size_t{1} << 23));
    }
    if (!queried) {
        return {0, 0};
    }

    constexpr uintptr_t page_size = GuestPageSize;
    const uintptr_t backing_begin = reinterpret_cast<uintptr_t>(backing_base);
    const uintptr_t view_begin = reinterpret_cast<uintptr_t>(impl->virtual_base);
    const size_t count = std::min(size_t{entries[0]}, entries.size() - 1);
    std::vector<uintptr_t> backing_pages;
    std::vector<uintptr_t> view_pages;
    backing_pages.reserve(count / 2);
    view_pages.reserve(count / 2);
    for (size_t i = 1; i <= count; ++i) {
        const uintptr_t page = entries[i] & ~(page_size - 1);
        if (page - backing_begin < backing_size) {
            backing_pages.push_back(page);
        } else if (view_begin != 0 && page - view_begin < impl->virtual_size) {
            view_pages.push_back(page);
        }
    }

    // Read only through the permanent writable backing mapping. A guest alias
    // can be unmapped or reprotected concurrently. VirtualUnlock discards no
    // guest bytes: a racing write stays valid and might merely incur a soft fault.
    const auto is_zero_page = [](const u8* data) {
        const auto* words = reinterpret_cast<const u64*>(data);
        // Lichii #1580: exit after the first non-zero cache line, which avoids
        // reading the remaining 4 KiB for almost every non-zero guest page.
        constexpr size_t WordsPerCacheLine = 64 / sizeof(u64);
        for (size_t i = 0; i < GuestPageSize / sizeof(u64); i += WordsPerCacheLine) {
            u64 nonzero = 0;
            for (size_t j = 0; j < WordsPerCacheLine; ++j) {
                nonzero |= words[i + j];
            }
            if (nonzero != 0) {
                return false;
            }
        }
        return true;
    };
    std::sort(view_pages.begin(), view_pages.end());
    // The working-set snapshot is no longer needed. Reuse its capacity for trim candidates.
    auto trim = std::move(entries);
    trim.clear();
    // Cache physical page checks so pages resident in both backing_base and virtual_base
    // are read at most once per pass, avoiding redundant memory traffic and binary searches.
    constexpr u8 PageChecked = 1;
    constexpr u8 PageZero = 2;
    const size_t total_backing_pages = backing_size / page_size;
    std::vector<u8> page_state(total_backing_pages, 0);
    size_t zero_bytes = 0;
    for (const uintptr_t page : backing_pages) {
        const size_t pfn = (page - backing_begin) / page_size;
        if (pfn >= total_backing_pages) {
            continue;
        }
        if (is_zero_page(reinterpret_cast<const u8*>(page))) {
            page_state[pfn] = PageChecked | PageZero;
            trim.push_back(page);
            zero_bytes += page_size;
        } else {
            page_state[pfn] = PageChecked;
        }
    }
    size_t covered_begin = 0;
    size_t covered_end = 0;
    size_t covered_physical = 0;
    for (const uintptr_t page : view_pages) {
        const size_t view_offset = page - view_begin;
        if (view_offset < covered_begin || view_offset >= covered_end) {
            const auto range = impl->BackingRange(view_offset);
            if (!range) {
                covered_end = 0;
                continue;
            }
            covered_begin = view_offset;
            // Limit stale mapping snapshots to 2 MiB before checking again.
            covered_end = std::min(range->second, view_offset + (size_t{2} << 20));
            covered_physical = range->first;
        }
        const size_t physical = covered_physical + view_offset - covered_begin;
        if (physical >= backing_size || backing_size - physical < page_size) {
            continue;
        }
        if (!impl->IsBackingCommitted(physical)) {
            continue;
        }
        const size_t pfn = physical / page_size;
        const uintptr_t physical_page = backing_begin + physical;
        u8 state = page_state[pfn];
        if ((state & PageChecked) == 0) {
            const bool zero = is_zero_page(reinterpret_cast<const u8*>(physical_page));
            state = static_cast<u8>(PageChecked | (zero ? PageZero : 0));
            page_state[pfn] = state;
            // The inspection itself can make a physical page resident. Evict it
            // again since it was not present in the original working-set scan.
            trim.push_back(physical_page);
        }
        if ((state & PageZero) != 0) {
            trim.push_back(page);
            zero_bytes += page_size;
        }
    }

    std::sort(trim.begin(), trim.end());
    trim.erase(std::unique(trim.begin(), trim.end()), trim.end());
    for (size_t first = 0; first < trim.size();) {
        size_t last = first + 1;
        // At most 1 MiB per unlock, so no single call holds the working set long (Lichii
        // 2b37c841fa, from Fritchii)
        constexpr size_t MaxUnlockBytes = size_t{1} << 20;
        while (last < trim.size() && trim[last] == trim[last - 1] + page_size &&
               (last - first) * page_size < MaxUnlockBytes) {
            ++last;
        }
        const uintptr_t begin = trim[first];
        const size_t length = trim[last - 1] + page_size - begin;
        if (!VirtualUnlock(reinterpret_cast<void*>(begin), length) &&
            GetLastError() != ERROR_NOT_LOCKED) {
            // A run may span two separate mapped views. Retry per page rather
            // than assuming that VirtualUnlock accepts it as one region.
            for (size_t i = first; i < last; ++i) {
                VirtualUnlock(reinterpret_cast<void*>(trim[i]), page_size);
            }
        }
        first = last;
    }
    return {(backing_pages.size() + view_pages.size()) * page_size, zero_bytes};
#else
    return {0, 0};
#endif
}

u64 HostMemory::MappedBytes() const noexcept {
#ifdef _WIN32
    return impl ? impl->MappedBytes() : 0;
#else
    return 0;
#endif
}

void HostMemory::SetTrimActivityCallback([[maybe_unused]] std::function<void()> callback) {
#ifdef _WIN32
    if (impl) {
        impl->SetTrimActivityCallback(std::move(callback));
    }
#endif
}

void HostMemory::EnableDirectMappedAddress() {
#if defined(__APPLE__)
    // Guest addresses are host addresses: the offsets passed to Map are guest addresses.
    if (impl && GetHostPageSize() != GuestPageSize) {
        if (u8* nce_base = impl->EnableNceDirectMap()) {
            virtual_base = nce_base;
            virtual_base_offset = 0;
            virtual_size = 1ULL << 39;
        }
        return;
    }
#endif
    if (impl) {
        impl->EnableDirectMappedAddress();
        virtual_size += reinterpret_cast<uintptr_t>(virtual_base);
    }
}

void HostMemory::ProtectAccess(size_t virtual_offset, size_t length, MemoryPermission allowed) {
#if defined(__APPLE__)
    if (impl && impl->nce_map) {
        impl->nce_map->SetAccessMask(virtual_offset, length, static_cast<u32>(allowed));
        return;
    }
#endif
    Protect(virtual_offset, length, allowed);
}

bool HostMemory::UsesNce() const {
#if defined(__APPLE__)
    return impl && impl->nce_map;
#else
    return false;
#endif
}

bool HostMemory::NceAccessAllowed(u64 address, bool write) const {
#if defined(__APPLE__)
    return impl && impl->nce_map && impl->nce_map->AccessAllowed(address, write);
#else
    return false;
#endif
}

bool HostMemory::NceHostAccessAllowed(u64 address, bool write) const {
#if defined(__APPLE__)
    return impl && impl->nce_map && impl->nce_map->HostAccessAllowed(address, write);
#else
    return false;
#endif
}

bool HostMemory::NceTrackedAccessAllowed(u64 address, bool write) const {
#if defined(__APPLE__)
    return impl && impl->nce_map && impl->nce_map->TrackedAccessAllowed(address, write);
#else
    return false;
#endif
}

const void* HostMemory::GetNceAtomicTable() const {
#if defined(__APPLE__)
    return impl && impl->nce_map ? impl->nce_map->AtomicTable() : nullptr;
#else
    return nullptr;
#endif
}

size_t HostMemory::GetNceWindows(u64* starts, u64* ends, size_t max) const {
#if defined(__APPLE__)
    if (!impl || !impl->nce_map) {
        return 0;
    }
    const auto& windows = impl->nce_map->Windows();
    for (size_t i = 0; i < windows.size() && i < max; i++) {
        starts[i] = windows[i].start;
        ends[i] = windows[i].end;
    }
    return windows.size();
#else
    return 0;
#endif
}

} // namespace Common
