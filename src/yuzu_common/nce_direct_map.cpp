// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#if defined(__APPLE__)

#include <algorithm>
#include <limits>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>

#include "yuzu_common/alignment.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/nce_direct_map.h"
#include "yuzu_common/yuzu_assert.h"

namespace Common {

namespace {

constexpr u64 GiB = 1ULL << 30;

// The Darwin kernel reserves [63 GiB, 448 GiB) for itself and maps nothing below 4 GiB.
constexpr u64 LowWindowFloor = 4 * GiB;
constexpr u64 LowWindowCeiling = 63 * GiB;
constexpr u64 HighWindowStart = 448 * GiB;
constexpr u64 HighWindowEnd = 512 * GiB;
constexpr u64 WindowAlignment = 2ULL << 20;

// Host page state. Zero is the reservation left by Reserve.
constexpr u64 HostNone = 1;

bool ReserveRange(u64 start, u64 size) {
    mach_vm_address_t address = start;
    const kern_return_t kr =
        mach_vm_map(mach_task_self(), &address, size, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0,
                    FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_NONE);
    return kr == KERN_SUCCESS && address == start;
}

template <typename T>
T* AllocateTable(size_t count) {
    void* table = mmap(nullptr, count * sizeof(T), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                       -1, 0);
    return table == MAP_FAILED ? nullptr : static_cast<T*>(table);
}

// Page-table operations may cover the whole guest address space, including
// holes outside the native reservations. Visit only their intersections with
// the two windows; the original address is retained for relative backing offsets.
template <typename States, typename Func>
void ForEachWindowRange(States& states, u64 address, u64 size, Func&& visit) {
    if (size > std::numeric_limits<u64>::max() - address) {
        LOG_ERROR(HW_Memory, "NCE: overflowing guest range at {:#x}, size {:#x}", address, size);
        return;
    }
    const u64 end = address + size;
    for (auto& state : states) {
        const u64 begin = std::max(address, state.window.start);
        const u64 clipped_end = std::min(end, state.window.end);
        if (begin < clipped_end) {
            visit(state, begin, clipped_end);
        }
    }
}

} // namespace

NceDirectMap::NceDirectMap(mach_port_t backing_entry, u8* base)
    : backing_base(base), entry(backing_entry) {}

NceDirectMap::~NceDirectMap() {
    if (atomic_table) munmap(atomic_table, AtomicTableSize);
    for (const WindowState& state : states) {
        munmap(reinterpret_cast<void*>(state.window.start), state.window.end - state.window.start);
        munmap(state.guest, state.guest_count * sizeof(u64));
        munmap(state.host, state.guest_count / 4 * sizeof(u64));
    }
}

bool NceDirectMap::Reserve(u64 min_low_size) {
    // The largest free range of the low window. Another thread may map into it between the
    // scan and the reservation, so retry a few times.
    Window low{};
    for (int attempt = 0; attempt < 4 && low.end == 0; attempt++) {
        Window best{};
        mach_vm_address_t address = LowWindowFloor;
        while (address < LowWindowCeiling) {
            mach_vm_address_t region = address;
            mach_vm_size_t region_size = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            const kern_return_t kr =
                mach_vm_region(mach_task_self(), &region, &region_size, VM_REGION_BASIC_INFO_64,
                               reinterpret_cast<vm_region_info_t>(&info), &count, &object);
            const u64 hole_end = kr == KERN_SUCCESS ? std::min<u64>(region, LowWindowCeiling)
                                                    : LowWindowCeiling;
            const u64 start = AlignUp(address, WindowAlignment);
            const u64 end = AlignDown(hole_end, WindowAlignment);
            if (end > start && end - start > best.end - best.start) {
                best = {start, end};
            }
            if (kr != KERN_SUCCESS) {
                break;
            }
            address = region + region_size;
        }
        if (best.end - best.start < min_low_size) {
            LOG_ERROR(HW_Memory, "NCE: largest free range below 63 GiB is {:#x} bytes",
                      best.end - best.start);
            return false;
        }
        if (ReserveRange(best.start, best.end - best.start)) {
            low = best;
        }
    }
    if (low.end == 0) {
        return false;
    }
    if (!ReserveRange(HighWindowStart, HighWindowEnd - HighWindowStart)) {
        LOG_ERROR(HW_Memory, "NCE: [448 GiB, 512 GiB) is not free");
        munmap(reinterpret_cast<void*>(low.start), low.end - low.start);
        return false;
    }

    for (const Window& window : {low, Window{HighWindowStart, HighWindowEnd}}) {
        WindowState state{};
        state.window = window;
        state.guest_count = (window.end - window.start) / GuestPageSize;
        state.guest = AllocateTable<std::atomic<u64>>(state.guest_count);
        state.host = AllocateTable<u64>(state.guest_count / 4);
        ASSERT(state.guest != nullptr && state.host != nullptr);
        windows.push_back(window);
        states.push_back(state);
    }
    if (backing_base) {
        mach_vm_address_t table = 0;
        const auto result = mach_vm_map(mach_task_self(), &table, AtomicTableSize,
            AtomicTableSize - 1, VM_FLAGS_ANYWHERE, MEMORY_OBJECT_NULL, 0, FALSE,
            VM_PROT_READ | VM_PROT_WRITE, VM_PROT_ALL, VM_INHERIT_NONE);
        if (result != KERN_SUCCESS) return false;
        atomic_table = reinterpret_cast<std::atomic<u64>*>(table);
    }
    LOG_INFO(HW_Memory, "NCE: guest windows [{:#x}, {:#x}) and [{:#x}, {:#x})", low.start, low.end,
             HighWindowStart, HighWindowEnd);
    return true;
}

bool NceDirectMap::Contains(u64 address, u64 size) const {
    for (const Window& window : windows) {
        if (address >= window.start && address <= window.end && size <= window.end - address) {
            return true;
        }
    }
    return false;
}

std::atomic<u64>* NceDirectMap::GuestEntry(u64 address) const {
    for (const WindowState& state : states) {
        if (address >= state.window.start && address < state.window.end) {
            return &state.guest[(address - state.window.start) / GuestPageSize];
        }
    }
    return nullptr;
}

void NceDirectMap::Map(u64 address, u64 backing_offset, u64 size, u32 perm) {
    std::scoped_lock lk{mutex};
    ForEachWindowRange(states, address, size, [&](WindowState& state, u64 begin, u64 end) {
        for (u64 page = begin; page < end; page += GuestPageSize) {
            auto& guest = state.guest[(page - state.window.start) / GuestPageSize];
            guest.store(Mapped | (backing_offset + page - address) | (perm & PermMask),
                        std::memory_order_relaxed);
        }
        Update(state, begin, end);
    });
}

void NceDirectMap::Unmap(u64 address, u64 size) {
    std::scoped_lock lk{mutex};
    ForEachWindowRange(states, address, size, [&](WindowState& state, u64 begin, u64 end) {
        for (u64 page = begin; page < end; page += GuestPageSize) {
            state.guest[(page - state.window.start) / GuestPageSize].store(
                0, std::memory_order_relaxed);
        }
        Update(state, begin, end);
    });
}

void NceDirectMap::Protect(u64 address, u64 size, u32 perm) {
    std::scoped_lock lk{mutex};
    ForEachWindowRange(states, address, size, [&](WindowState& state, u64 begin, u64 end) {
        for (u64 page = begin; page < end; page += GuestPageSize) {
            auto& guest = state.guest[(page - state.window.start) / GuestPageSize];
            const u64 value = guest.load(std::memory_order_relaxed);
            if ((value & Mapped) != 0) {
                guest.store((value & ~PermMask) | (perm & PermMask), std::memory_order_relaxed);
            }
        }
        Update(state, begin, end);
    });
}

void NceDirectMap::SetAccessMask(u64 address, u64 size, u32 allowed) {
    // Only reads and writes are ever denied: execution needs no tracking.
    const u64 denied = static_cast<u64>(~allowed & (PermRead | PermWrite)) << DenyShift;
    std::scoped_lock lk{mutex};
    ForEachWindowRange(states, address, size, [&](WindowState& state, u64 begin, u64 end) {
        for (u64 page = begin; page < end; page += GuestPageSize) {
            auto& guest = state.guest[(page - state.window.start) / GuestPageSize];
            const u64 value = guest.load(std::memory_order_relaxed);
            if ((value & Mapped) != 0) {
                guest.store((value & ~(PermMask << DenyShift)) | denied,
                            std::memory_order_release);
            }
        }
        Update(state, begin, end);
    });
}

bool NceDirectMap::AccessAllowed(u64 address, bool write) const {
    const std::atomic<u64>* guest = GuestEntry(address);
    if (guest == nullptr) {
        return false;
    }
    const u64 value = guest->load(std::memory_order_relaxed);
    return (value & Mapped) != 0 && (value & (write ? PermWrite : PermRead)) != 0;
}

bool NceDirectMap::TrackedAccessAllowed(u64 address, bool write) const {
    const auto* guest = GuestEntry(address);
    if (!guest) return false;
    const u64 value = guest->load(std::memory_order_acquire);
    const u64 permission = write ? PermWrite : PermRead;
    return (value & Mapped) != 0 && (value & permission) != 0 &&
           ((value >> DenyShift) & permission) == 0;
}

bool NceDirectMap::HostAccessAllowed(u64 address, bool write) const {
    for (const WindowState& state : states) {
        if (address >= state.window.start && address < state.window.end) {
            const u64 value = std::atomic_ref<u64>{
                state.host[(address - state.window.start) / HostPageSize]}
                                  .load(std::memory_order_relaxed);
            return (value & Mapped) != 0 && (value & (write ? PermWrite : PermRead)) != 0;
        }
    }
    return false;
}

void NceDirectMap::Update(WindowState& state, u64 begin, u64 end) {
    if (atomic_table) {
        for (u64 page = begin; page < end; page += GuestPageSize) {
            const u64 value = state.guest[(page - state.window.start) / GuestPageSize].load(
                std::memory_order_relaxed);
            u64 translated = 0;
            if (value & Mapped) {
                const u64 permissions = (value & PermMask) & ~((value >> DenyShift) & PermMask);
                translated = (reinterpret_cast<u64>(backing_base) + (value & OffsetMask) - page)
                             | (permissions & (PermRead | PermWrite));
            }
            atomic_table[page / GuestPageSize].store(translated, std::memory_order_release);
        }
    }
    UpdateWindow(state, begin, end);
}

void NceDirectMap::UpdateWindow(WindowState& state, u64 begin, u64 end) {
    enum class Action { None, Clear, Remap, Protect };
    struct Run {
        Action action{Action::None};
        u64 address{};
        u64 size{};
        u64 offset{};
        u32 prot{};
    } run;

    const auto flush = [&] {
        if (run.action == Action::None) {
            return;
        }
        mach_vm_address_t address = run.address;
        kern_return_t kr = KERN_SUCCESS;
        switch (run.action) {
        case Action::Clear:
            kr = mach_vm_map(mach_task_self(), &address, run.size, 0,
                             VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, MEMORY_OBJECT_NULL, 0, FALSE,
                             VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_NONE);
            break;
        case Action::Remap:
            kr = mach_vm_map(mach_task_self(), &address, run.size, 0,
                             VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry, run.offset, FALSE,
                             static_cast<vm_prot_t>(run.prot), VM_PROT_ALL, VM_INHERIT_NONE);
            break;
        case Action::Protect:
            kr = mach_vm_protect(mach_task_self(), address, run.size, FALSE,
                                 static_cast<vm_prot_t>(run.prot));
            break;
        case Action::None:
            break;
        }
        ASSERT_MSG(kr == KERN_SUCCESS, "NCE: host mapping of {:#x} bytes at {:#x} failed: {}",
                   run.size, run.address, kr);
        run = {};
    };

    const u64 first = AlignDown(begin, HostPageSize);
    const u64 last = AlignUp(end, HostPageSize);
    for (u64 host_page = first; host_page < last; host_page += HostPageSize) {
        const size_t index = (host_page - state.window.start) / HostPageSize;
        const std::atomic<u64>* guest = &state.guest[index * 4];

        // Desired state of the host page.
        u64 desired = HostNone;
        u64 base = ~0ULL;
        u32 perm = 0;
        u32 denied = 0;
        bool direct = true;
        for (u64 i = 0; i < 4; i++) {
            const u64 value = guest[i].load(std::memory_order_relaxed);
            if ((value & Mapped) == 0) {
                continue;
            }
            const u64 page_base = (value & OffsetMask) - i * GuestPageSize;
            if (base == ~0ULL) {
                base = page_base;
            }
            direct &= page_base == base && (base % HostPageSize) == 0;
            perm |= static_cast<u32>(value & PermMask);
            denied |= static_cast<u32>((value >> DenyShift) & PermMask);
        }
        if (base != ~0ULL && direct) {
            u32 prot = perm & ~denied;
            if ((prot & PermWrite) != 0 && (prot & PermExecute) != 0) {
                prot &= ~PermWrite;
            }
            // The host has no execute-only pages; ask for read explicitly so that both mapping
            // paths give the same protection.
            if ((prot & PermExecute) != 0) {
                prot |= PermRead;
            }
            desired = Mapped | base | prot;
        }

        const u64 current = state.host[index] == 0 ? HostNone : state.host[index];
        Action action = Action::None;
        if (desired != current) {
            if (desired == HostNone) {
                action = Action::Clear;
            } else if (current != HostNone && (current & OffsetMask) == (desired & OffsetMask)) {
                action = Action::Protect;
            } else {
                action = Action::Remap;
            }
        }
        state.host[index] = desired;

        const u32 prot = static_cast<u32>(desired & PermMask);
        const u64 offset = desired & OffsetMask;
        const bool extends = action == run.action && run.address + run.size == host_page &&
                             (action == Action::Clear ||
                              (prot == run.prot &&
                               (action == Action::Protect || offset == run.offset + run.size)));
        if (!extends) {
            flush();
            if (action != Action::None) {
                run = {action, host_page, 0, offset, prot};
            }
        }
        if (action != Action::None) {
            run.size += HostPageSize;
        }
    }
    flush();
}

} // namespace Common

#endif
