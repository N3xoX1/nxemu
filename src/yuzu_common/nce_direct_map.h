// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(__APPLE__)

#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

#include <mach/mach_types.h>

#include "yuzu_common/common_types.h"

namespace Common {

/**
 * Maps guest memory at the same address in the host for native code execution on hosts with
 * 16 KiB pages. Each host page holds four guest pages; it is mapped directly when the backing
 * of its mapped guest pages is one aligned 16 KiB block, with the union of their permissions,
 * minus write when it would be writable and executable, minus what the access mask of any of
 * them denies. Otherwise it is inaccessible. Accesses the host page refuses but the guest page
 * allows are emulated by the fault handler.
 */
class NceDirectMap {
public:
    static constexpr u64 GuestPageSize = 0x1000;
    static constexpr u64 HostPageSize = 0x4000;

    // Permission and access mask bits, as Common::MemoryPermission.
    static constexpr u32 PermRead = 1;
    static constexpr u32 PermWrite = 2;
    static constexpr u32 PermExecute = 4;

    struct Window {
        u64 start;
        u64 end;
    };

    explicit NceDirectMap(mach_port_t backing_entry, u8* backing_base = nullptr);
    const void* AtomicTable() const { return atomic_table; }
    ~NceDirectMap();

    NceDirectMap(const NceDirectMap&) = delete;
    NceDirectMap& operator=(const NceDirectMap&) = delete;

    /// Reserves the host windows that hold guest memory: the largest free range below the
    /// kernel reservation that starts at 63 GiB, of at least min_low_size bytes, and
    /// [448 GiB, 512 GiB). Returns false if they are not available.
    bool Reserve(u64 min_low_size);

    /// Low window first, then high window.
    const std::vector<Window>& Windows() const {
        return windows;
    }

    bool Contains(u64 address, u64 size) const;

    /// Operations are clipped to the reserved windows. Ranges may span holes;
    /// wrapping ranges are rejected before any state changes.
    void Map(u64 address, u64 backing_offset, u64 size, u32 perm);
    void Unmap(u64 address, u64 size);
    void Protect(u64 address, u64 size, u32 perm);

    /// Limits host access to [address, address + size) to `allowed`, independently of the guest
    /// permission. Used for pages the GPU caches and debugger watchpoints.
    void SetAccessMask(u64 address, u64 size, u32 allowed);

    /// Whether the guest permission of the page holding `address` allows the access. Safe to
    /// call from a signal handler.
    bool AccessAllowed(u64 address, bool write) const;

    /// Guest permission AND GPU access mask of this 4 KiB page. Unlike the host
    /// permission, this is unaffected by a differently protected 4 KiB neighbor.
    bool TrackedAccessAllowed(u64 address, bool write) const;

    /// Whether the host page holding `address` allows the access, or is about to. Safe to call
    /// from a signal handler.
    bool HostAccessAllowed(u64 address, bool write) const;

private:
    // Guest page entry: backing offset (page aligned) | Mapped | perm | denied << DenyShift.
    static constexpr u64 Mapped = 1ULL << 63;
    static constexpr u64 PermMask = 7;
    static constexpr u64 DenyShift = 3;
    static constexpr u64 OffsetMask = 0x0000'FFFF'FFFF'F000ULL;

    struct WindowState {
        Window window;
        std::atomic<u64>* guest{};  // one entry per guest page
        u64* host{};                // current host page state, see HostState()
        size_t guest_count{};
    };

    std::atomic<u64>* GuestEntry(u64 address) const;
    void Update(WindowState& state, u64 begin, u64 end);
    void UpdateWindow(WindowState& state, u64 begin, u64 end);

    // One canonical pointer/permission entry per guest page. Aligning the table to its size
    // permits index formation without borrowing a second register for its base address.
    static constexpr u64 AtomicTableSize = (1ULL << (39 - 12)) * sizeof(u64);
    std::atomic<u64>* atomic_table{};
    u8* backing_base{};
    mach_port_t entry;
    std::vector<Window> windows;
    std::vector<WindowState> states;
    std::mutex mutex;
};

} // namespace Common

#endif
