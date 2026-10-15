// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Architecture based in part on Ryubing/Ryujinx AppleHv (MIT):
// Copyright (c) Ryujinx Team and Contributors.

#include "apple_hv/apple_hv_address_space.h"

#include "apple_hv/optimization_flags.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>
#include <utility>

#include <fmt/format.h>

#include <Hypervisor/Hypervisor.h>
#include <sys/mman.h>
#include <unistd.h>

#include "yuzu_common/alignment.h"
#include "yuzu_common/host_memory.h"
#include "yuzu_common/logging/log.h"
#include <nxemu-module-spec/cpu.h>
#include <nxemu-module-spec/operating_system.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

namespace Core::AppleHv {
namespace {

constexpr u64 GuestPageSize = 0x1000;
constexpr u64 DramBase = 0x80000000ULL;
constexpr u64 HostAllocationGranule = 0x4000;
constexpr u64 IpaRegionSize = 1ULL << 35;
constexpr u64 RamIpaAlignment = 1ULL << 30;
constexpr u64 KernelRegionBase = static_cast<u64>(-static_cast<s64>(1ULL << 39));
constexpr u64 KernelCodeSize = 0x2000;
constexpr u64 KernelTlbiEretOffset = 0x1000;
constexpr u64 KernelEretOffset = 0x1020;
constexpr u64 KernelTlbOnlyEretOffset = 0x1040;

constexpr u64 LevelBits = 9;
constexpr u64 LevelMask = (1ULL << LevelBits) - 1;

// AArch64 stage-1 page descriptor attributes. These mirror the small subset used by Ryubing.
constexpr u64 AccessFlag = 1ULL << 10;
constexpr u64 InnerShareable = 3ULL << 8;
constexpr u64 ApShift = 6;
constexpr u64 Pxn = 1ULL << 53;
constexpr u64 Uxn = 1ULL << 54;
constexpr u64 AttributeMask = (0x3ffULL << 2) | (0x3fffULL << 50);

[[noreturn]] void ThrowHvError(const char* operation, hv_return_t result) {
    throw std::runtime_error(fmt::format("{} failed with Hypervisor.framework status {:#x}",
                                         operation, static_cast<u32>(result)));
}

void CheckHv(const char* operation, hv_return_t result) {
    if (result != HV_SUCCESS) {
        ThrowHvError(operation, result);
    }
}

class VmState {
public:
    void Acquire() {
        std::scoped_lock lk{m_mutex};
        if (m_ref_count++ == 0) {
            const hv_return_t result = hv_vm_create(nullptr);
            if (result != HV_SUCCESS) {
                --m_ref_count;
                ThrowHvError("hv_vm_create", result);
            }
            LOG_INFO(Core_ARM, "AppleHv: hv_vm_create succeeded");
            m_next_ipa = HostAllocationGranule;
            m_free_ranges.clear();
        }
    }

    void Release() noexcept {
        std::scoped_lock lk{m_mutex};
        if (--m_ref_count == 0) {
            const hv_return_t result = hv_vm_destroy();
            if (result != HV_SUCCESS) {
                LOG_ERROR(Core_ARM, "hv_vm_destroy failed with status {:#x}", static_cast<u32>(result));
            }
            m_next_ipa = HostAllocationGranule;
            m_free_ranges.clear();
        }
    }

    u64 AllocateIpa(u64 size, u64 alignment = HostAllocationGranule) {
        std::scoped_lock lk{m_mutex};
        size = Common::AlignUp(size, HostAllocationGranule);
        for (size_t i = 0; i < m_free_ranges.size(); ++i) {
            const auto range = m_free_ranges[i];
            const u64 start = Common::AlignUp(range.start, alignment);
            const u64 end = start + size;
            if (end < start || end > range.end) {
                continue;
            }
            m_free_ranges.erase(m_free_ranges.begin() + static_cast<ptrdiff_t>(i));
            if (range.start < start) {
                m_free_ranges.push_back({range.start, start});
            }
            if (end < range.end) {
                m_free_ranges.push_back({end, range.end});
            }
            CoalesceFreeRangesLocked();
            return start;
        }

        const u64 start = Common::AlignUp(m_next_ipa, alignment);
        const u64 end = start + size;
        if (end < start || end > IpaRegionSize) {
            throw std::runtime_error("AppleHv IPA address space exhausted");
        }
        m_next_ipa = end;
        return start;
    }

    void FreeIpa(u64 address, u64 size) noexcept {
        if (address == 0 || size == 0) {
            return;
        }
        std::scoped_lock lk{m_mutex};
        const u64 end = address + Common::AlignUp(size, HostAllocationGranule);
        if (end <= address || end > IpaRegionSize) {
            return;
        }
        m_free_ranges.push_back({address, end});
        CoalesceFreeRangesLocked();
    }

private:
    struct FreeRange {
        u64 start{};
        u64 end{};
    };

    void CoalesceFreeRangesLocked() {
        std::ranges::sort(m_free_ranges, {}, &FreeRange::start);
        size_t out = 0;
        for (const auto range : m_free_ranges) {
            if (out != 0 && range.start <= m_free_ranges[out - 1].end) {
                m_free_ranges[out - 1].end = std::max(m_free_ranges[out - 1].end, range.end);
            } else {
                m_free_ranges[out++] = range;
            }
        }
        m_free_ranges.resize(out);
    }

    std::mutex m_mutex;
    u32 m_ref_count{};
    u64 m_next_ipa{HostAllocationGranule};
    std::vector<FreeRange> m_free_ranges;
};

VmState& GetVmState() {
    static VmState vm;
    return vm;
}

u64 PermissionAttributes(Common::MemoryPermission permissions, bool kernel_mapping = false) {
    const bool read = True(permissions & Common::MemoryPermission::Read);
    const bool write = True(permissions & Common::MemoryPermission::Write);
    const bool execute = True(permissions & Common::MemoryPermission::Execute);

    if (kernel_mapping) {
        // EL0 no access, EL1 read/execute. UXN blocks EL0 execution; PXN stays clear.
        return AccessFlag | InnerShareable | Uxn | (2ULL << ApShift);
    }

    // AP[2:1] encoding used here:
    // 01 = EL0/EL1 RW, 10 = EL1 RO / EL0 none, 11 = EL0/EL1 RO.
    u64 attributes = AccessFlag | InnerShareable;
    if (write) {
        attributes |= 1ULL << ApShift;
    } else if (read) {
        attributes |= 3ULL << ApShift;
    } else {
        attributes |= 2ULL << ApShift;
    }

    // Never execute guest user pages at EL1. UXN controls EL0 execution.
    attributes |= Pxn;
    if (!execute) {
        attributes |= Uxn;
    }
    return attributes;
}

} // namespace

class AddressSpace::Impl {
public:
    struct TableBlock {
        u8* host{};
        u64 ipa{};
        u32 used_slots{};
    };

    struct TableStorage {
        u64* entries{};
        u64 ipa{};
    };

    struct PageAttributes {
        Common::MemoryPermission permissions{};
        Common::MemoryPermission tracking{Common::MemoryPermission::ReadWrite};

        u64 Hardware() const {
            return PermissionAttributes(permissions & (tracking | Common::MemoryPermission::Execute));
        }
    };

    struct Level {
        TableStorage storage{};
        std::array<Level*, 512> next{};
        std::array<PageAttributes, 512> attributes{};
    };

    Impl(IDeviceMemory& device_memory, u32 address_space_bits)
        : m_address_space_bits{address_space_bits},
          m_user_address_limit{address_space_bits <= 39 ? 1ULL << address_space_bits : 0} {
        if (address_space_bits != 36 && address_space_bits != 39) {
            throw std::invalid_argument("AppleHv requires a 36-bit or 39-bit user address space");
        }
        auto& vm = GetVmState();
        vm.Acquire();
        m_vm_acquired = true;

        try {
            m_ram_host = const_cast<u8*>(device_memory.BackingBasePointer());
            m_ram_size = device_memory.BackingSize();

            const long host_page_size = sysconf(_SC_PAGESIZE);
            if (host_page_size <= 0 || !m_ram_host || m_ram_size == 0 ||
                (reinterpret_cast<uintptr_t>(m_ram_host) % static_cast<u64>(host_page_size)) != 0 ||
                (m_ram_size % static_cast<u64>(host_page_size)) != 0) {
                throw std::runtime_error("DeviceMemory backing is not aligned for hv_vm_map");
            }

            m_ram_ipa = vm.AllocateIpa(m_ram_size, RamIpaAlignment);
            const hv_return_t ram_map_result =
                hv_vm_map(m_ram_host, m_ram_ipa, m_ram_size,
                          static_cast<hv_memory_flags_t>(HV_MEMORY_READ | HV_MEMORY_WRITE |
                                                         HV_MEMORY_EXEC));
            if (ram_map_result != HV_SUCCESS) {
                vm.FreeIpa(m_ram_ipa, m_ram_size);
                m_ram_ipa = 0;
                ThrowHvError("hv_vm_map(DeviceMemory)", ram_map_result);
            }
            m_ram_mapped = true;

            m_user_root = NewLevel();
            m_kernel_root = NewLevel();
            InitializeKernelVector();
            m_valid = true;

            LOG_INFO(
                Core_ARM,
                "AppleHv address space initialized: {}-bit user VA, RAM host={} size={:#x} IPA={:#x}",
                m_address_space_bits, static_cast<void*>(m_ram_host), m_ram_size, m_ram_ipa);
        } catch (...) {
            Cleanup();
            throw;
        }
    }

    ~Impl() {
        Cleanup();
    }

    u64 Identity() const noexcept { return m_identity; }

    bool IsValid() const noexcept {
        return m_valid;
    }

    u64 CodeGeneration() const noexcept { return m_code_generation.load(std::memory_order_acquire); }
    u64 KernelTlbOnlyEretAddress() const noexcept { return KernelRegionBase + KernelTlbOnlyEretOffset; }

    u64 Generation() const noexcept {
        return m_generation.load(std::memory_order_acquire);
    }

    u64 KernelEretAddress() const noexcept {
        return KernelRegionBase + KernelEretOffset;
    }

    u64 KernelTlbiEretAddress() const noexcept {
        return KernelRegionBase + KernelTlbiEretOffset;
    }

    bool InitializeVcpu(hv_vcpu_t vcpu) const {
        if (!m_valid) {
            return false;
        }

        const u64 tcr = (0x00000011B5193519ULL & ~0x3fULL) |
                        static_cast<u64>(64 - m_address_space_bits);
        return hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, KernelRegionBase) == HV_SUCCESS &&
               hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, m_user_root->storage.ipa) ==
                   HV_SUCCESS &&
               hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TTBR1_EL1, m_kernel_root->storage.ipa) ==
                   HV_SUCCESS &&
               hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, 0xffULL) == HV_SUCCESS &&
               hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, tcr) == HV_SUCCESS &&
               hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, 0x0000000034D5D925ULL) ==
                   HV_SUCCESS;
    }

    void RegisterVcpu(hv_vcpu_t vcpu, std::atomic_bool& running) {
        std::scoped_lock lk{m_mutex};
        m_active_vcpus.emplace_back(vcpu, &running);
    }

    void UnregisterVcpu(hv_vcpu_t vcpu) {
        std::scoped_lock lk{m_mutex};
        std::erase_if(m_active_vcpus, [vcpu](const auto& entry) { return entry.first == vcpu; });
    }

    std::shared_lock<std::shared_mutex> LockExecution(std::atomic_bool& running) {
        // Serialize entry with writers so a kicked vCPU cannot re-enter ahead of a pending edit.
        std::scoped_lock lk{m_mutex};
        std::shared_lock lock{m_execution_mutex};
        // Publish before releasing the entry mutex: a writer cannot miss a vCPU
        // about to execute and then wait on its execution gate without kicking it.
        running.store(true, std::memory_order_release);
        return lock;
    }

    void LogFault(u64 va) {
        std::scoped_lock lk{m_mutex};
        PageAttributes* attributes{};
        const u64* pte = FindPageEntry(m_user_root, va, false, &attributes);
        LOG_ERROR(Core_ARM, "AppleHv fault mapping: VA={:#x}, PTE={:#x}, guest permissions={}, GPU mask={}, generation={}",
            va, pte ? *pte : 0, attributes ? static_cast<u32>(attributes->permissions) : 0,
            attributes ? static_cast<u32>(attributes->tracking) : 0, Generation());
    }

    bool AllowsAccess(u64 address, Common::MemoryPermission access) {
        std::scoped_lock lk{m_mutex};
        if (address >= m_user_address_limit) {
            return false;
        }
        PageAttributes* attributes{};
        const u64* pte = FindPageEntry(m_user_root, address, false, &attributes);
        return pte && (*pte & 3) &&
            (attributes->permissions & (attributes->tracking | Common::MemoryPermission::Execute) & access) == access;
    }

    void InvalidateCode() {
        std::scoped_lock lk{m_mutex};
        StopVcpusLocked();
        std::unique_lock execution_lock{m_execution_mutex};
        m_code_generation.fetch_add(1, std::memory_order_release);
        NotifyMappingChangedLocked();
    }

    void Map(u64 va, u64 pa, u64 size, Common::MemoryPermission permissions) {
        std::scoped_lock lk{m_mutex};
        ValidateGuestRange(va, pa, size);
        StopVcpusLocked();
        std::unique_lock execution_lock{m_execution_mutex};
        MapRange(m_user_root, 0, va, m_ram_ipa + pa - DramBase,
                 size, {.permissions = permissions});
        if (True(permissions & Common::MemoryPermission::Execute)) {
            m_code_generation.fetch_add(1, std::memory_order_release);
        }
        NotifyMappingChangedLocked();
    }

    enum class Update { Unmap, Protect, Track };

    void Change(u64 va, u64 size, Update update, Common::MemoryPermission permissions) {
        std::scoped_lock lk{m_mutex};
        ValidateVirtualRange(va, size);
        // Repeated GPU notifications often request a mask already present. In that case
        // avoid both a VM exit and a TLB generation bump, without deferring protection.
        if (!NeedsUpdate(m_user_root, 0, va, size, update, permissions)) {
            return;
        }
        if (update == Update::Track && (OptimizationFlags() & RelaxPermissions) != 0 &&
            CanRelaxTracking(va, size, permissions)) {
            RelaxTracking(va, size, permissions);
            NotifyMappingChangedLocked();
            return;
        }
        StopVcpusLocked();
        std::unique_lock execution_lock{m_execution_mutex};
        UpdateRange(m_user_root, 0, va, size, update, permissions);
        if (update == Update::Protect && True(permissions & Common::MemoryPermission::Execute)) {
            m_code_generation.fetch_add(1, std::memory_order_release);
        }
        NotifyMappingChangedLocked();
    }

    void Unmap(u64 va, u64 size) {
        Change(va, size, Update::Unmap, {});
    }

    void Protect(u64 va, u64 size, Common::MemoryPermission permissions) {
        Change(va, size, Update::Protect, permissions);
    }

    void Track(u64 va, u64 size, Common::MemoryPermission permissions) {
        if (size == 0 || va + size < va) {
            return;
        }
        const u64 end = Common::AlignUp(va + size, GuestPageSize);
        va = Common::AlignDown(va, GuestPageSize);
        Change(va, end - va, Update::Track, permissions);
    }

    void TrackBatch(std::span<const Common::NativeMemoryTrackRange> ranges) {
        if (ranges.empty()) {
            return;
        }

        std::scoped_lock lk{m_mutex};

        // Validate the whole batch before changing any descriptor.
        u64 valid_ranges = 0;
        for (const auto& range : ranges) {
            const u64 va = range.virtual_address.GetValue();
            if (range.size == 0 || va + range.size < va) {
                continue;
            }
            const u64 start = Common::AlignDown(va, GuestPageSize);
            const u64 end = Common::AlignUp(va + range.size, GuestPageSize);
            ValidateVirtualRange(start, end - start);
            ++valid_ranges;
        }
        if (valid_ranges == 0) {
            return;
        }


        // Only disjoint, increasing ranges can be relaxed together without exposing an
        // intermediate permission that a later overlapping range removes again.
        if ((OptimizationFlags() & RelaxPermissions) != 0) {
            bool can_relax = true;
            bool changed = false;
            u64 previous_end = 0;
            u64 pages = 0;
            for (const auto& range : ranges) {
                const u64 va = range.virtual_address.GetValue();
                if (range.size == 0 || va + range.size < va) continue;
                const u64 start = Common::AlignDown(va, GuestPageSize);
                const u64 end = Common::AlignUp(va + range.size, GuestPageSize);
                pages += (end - start) / GuestPageSize;
                if (pages > 64 || start < previous_end || !CanRelaxTracking(start, end - start, range.permissions)) {
                    can_relax = false;
                    break;
                }
                previous_end = end;
                changed |= NeedsUpdate(m_user_root, 0, start, end - start, Update::Track, range.permissions);
            }
            if (can_relax) {
                if (!changed) return;
                for (const auto& range : ranges) {
                    const u64 va = range.virtual_address.GetValue();
                    if (range.size == 0 || va + range.size < va) continue;
                    const u64 start = Common::AlignDown(va, GuestPageSize);
                    const u64 end = Common::AlignUp(va + range.size, GuestPageSize);
                    RelaxTracking(start, end - start, range.permissions);
                }
                NotifyMappingChangedLocked();
                return;
            }
        }

        std::unique_lock execution_lock{m_execution_mutex, std::defer_lock};
        for (const auto& range : ranges) {
            u64 va = range.virtual_address.GetValue();
            if (range.size == 0 || va + range.size < va) {
                continue;
            }
            const u64 end = Common::AlignUp(va + range.size, GuestPageSize);
            va = Common::AlignDown(va, GuestPageSize);
            // Evaluate against preceding edits, not the initial state: a later overlapping range
            // may restore permissions that an earlier range in this very batch removed.
            if (!NeedsUpdate(m_user_root, 0, va, end - va, Update::Track, range.permissions)) {
                continue;
            }
            if (!execution_lock.owns_lock()) {
                StopVcpusLocked();
                execution_lock.lock();
            }
            UpdateRange(m_user_root, 0, va, end - va, Update::Track, range.permissions);
        }
        if (execution_lock.owns_lock()) {
            // One generation covers all edits with the existing full TLB flush.
            NotifyMappingChangedLocked();
        }
    }

    u64 MappingGranule(u64 va) {
        std::scoped_lock lk{m_mutex};
        Level* level = m_user_root;
        for (u32 depth = 0; depth < 3; ++depth) {
            const u32 index = Index(va, depth);
            if (!(level->storage.entries[index] & 1)) {
                return 0;
            }
            if (!level->next[index]) {
                return BlockSize(depth);
            }
            level = level->next[index];
        }
        return 0;
    }

private:
    static u64 BlockSize(u32 depth) { return 1ULL << (30 - depth * 9); }
    static u32 Index(u64 va, u32 depth) { return (va >> (30 - depth * 9)) & LevelMask; }
    static constexpr u64 PhysicalMask = ((1ULL << 48) - 1) & ~0xfffULL;

    // Split a block into 512 contiguous child descriptors, retaining the exact guest
    // permissions and GPU tracking mask. No guest mappings are invented by a split.
    Level* Split(Level* level, u32 index, u32 depth) {
        if (level->next[index]) {
            return level->next[index];
        }
        Level* next = NewLevel();
        const u64 old = level->storage.entries[index];
        if (old & 1) {
            const u64 child_size = BlockSize(depth + 1);
            const u64 type = depth + 1 == 2 ? 3 : 1;
            for (u32 i = 0; i < 512; ++i) {
                next->attributes[i] = level->attributes[index];
                next->storage.entries[i] = (old & PhysicalMask) + i * child_size |
                    (old & AttributeMask) | type;
            }
        }
        level->next[index] = next;
        level->storage.entries[index] = next->storage.ipa | 3;
        return next;
    }

    void MapRange(Level* level, u32 depth, u64 va, u64 ipa, u64 size, PageAttributes attributes) {
        const u64 block_size = BlockSize(depth);
        while (size) {
            const u32 index = Index(va, depth);
            const u64 bytes = std::min(size, block_size - (va & (block_size - 1)));
            if (bytes == block_size && !(ipa & (block_size - 1)) && !level->next[index]) {
                level->attributes[index] = attributes;
                level->storage.entries[index] = ipa | attributes.Hardware() | (depth == 2 ? 3 : 1);
            } else {
                MapRange(Split(level, index, depth), depth + 1, va, ipa, bytes, attributes);
            }
            va += bytes;
            ipa += bytes;
            size -= bytes;
        }
    }

    bool NeedsUpdate(Level* level, u32 depth, u64 va, u64 size, Update update,
                     Common::MemoryPermission permissions) const {
        const u64 block_size = BlockSize(depth);
        while (size) {
            const u32 index = Index(va, depth);
            const u64 bytes = std::min(size, block_size - (va & (block_size - 1)));
            if (level->next[index]) {
                if (NeedsUpdate(level->next[index], depth + 1, va, bytes, update, permissions)) {
                    return true;
                }
            } else if (level->storage.entries[index] & 1) {
                const auto& attributes = level->attributes[index];
                if (update == Update::Unmap ||
                    (update == Update::Protect && attributes.permissions != permissions) ||
                    (update == Update::Track && attributes.tracking != permissions)) {
                    return true;
                }
            }
            va += bytes;
            size -= bytes;
        }
        return false;
    }

    bool CanRelaxTracking(u64 va, u64 size, Common::MemoryPermission tracking) {
        // Bound the preflight scan; large mappings use the existing block-aware path.
        if (size > 64 * GuestPageSize) return false;
        // Never split blocks, change physical backing, or remove a permission without
        // the execution gate. A stale TLB entry here can only be too restrictive.
        for (u64 offset = 0; offset < size; offset += GuestPageSize) {
            Level* level = m_user_root;
            for (u32 depth = 0; depth < 2; ++depth) {
                level = level->next[Index(va + offset, depth)];
                if (!level) return false;
            }
            const u32 index = Index(va + offset, 2);
            if ((level->storage.entries[index] & 3) != 3) return false;
            const auto& old = level->attributes[index];
            const auto old_access = old.permissions & (old.tracking | Common::MemoryPermission::Execute);
            const auto new_access = old.permissions & (tracking | Common::MemoryPermission::Execute);
            if ((old_access & new_access) != old_access) return false;
        }
        return true;
    }

    void RelaxTracking(u64 va, u64 size, Common::MemoryPermission tracking) {
        for (u64 offset = 0; offset < size; offset += GuestPageSize) {
            PageAttributes* attributes{};
            u64* pte = FindPageEntry(m_user_root, va + offset, false, &attributes);
            attributes->tracking = tracking;
            const u64 value = (*pte & ~AttributeMask) | attributes->Hardware();
            // Naturally aligned descriptors are published atomically while other cores
            // may walk them. AF is already set and hardware dirty-state updates are off.
            std::atomic_ref<u64>{*pte}.store(value, std::memory_order_release);
        }
    }

    void UpdateRange(Level* level, u32 depth, u64 va, u64 size, Update update,
                     Common::MemoryPermission permissions) {
        const u64 block_size = BlockSize(depth);
        while (size) {
            const u32 index = Index(va, depth);
            const u64 bytes = std::min(size, block_size - (va & (block_size - 1)));
            u64& pte = level->storage.entries[index];
            if (pte & 1) {
                if (level->next[index] || bytes != block_size) {
                    if (NeedsUpdate(level, depth, va, bytes, update, permissions)) {
                        UpdateRange(Split(level, index, depth), depth + 1, va, bytes, update, permissions);
                    }
                } else if (update == Update::Unmap) {
                    pte = 0;
                } else {
                    auto& attributes = level->attributes[index];
                    if (update == Update::Protect) {
                        attributes.permissions = permissions;
                    } else {
                        attributes.tracking = permissions;
                    }
                    pte = (pte & ~AttributeMask) | attributes.Hardware();
                }
            }
            va += bytes;
            size -= bytes;
        }
    }

private:
    void InitializeKernelVector() {
        m_kernel_code_host = static_cast<u8*>(
            mmap(nullptr, HostAllocationGranule, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (m_kernel_code_host == MAP_FAILED) {
            m_kernel_code_host = nullptr;
            throw std::runtime_error(fmt::format("mmap(kernel vector) failed: {}", strerror(errno)));
        }

        std::memset(m_kernel_code_host, 0, HostAllocationGranule);
        // Every AArch64 vector slot traps to the host with HVC #0xffff. The host then resumes
        // through one of the tiny EL1 stubs below so the architectural EL1 -> EL0 transition is
        // performed by ERET rather than by forcing PC/PSTATE from the host.
        for (u64 offset = 0; offset < 0x800; offset += 0x80) {
            const u32 hvc = 0xD41FFFE2;
            std::memcpy(m_kernel_code_host + offset, &hvc, sizeof(hvc));
        }

        // Proper architectural page-table maintenance sequence. The host mutates the stage-1
        // tables, then the vCPU executes this at EL1 before ERET back to guest EL0.
        constexpr std::array<u32, 6> tlbi_eret = {
            0xD5033A9F, // DSB ISHST
            0xD508871F, // TLBI VMALLE1: each vCPU observes and flushes its own generation
            0xD508751F, // IC IALLU: only used when executable code changed
            0xD5033B9F, // DSB ISH
            0xD5033FDF, // ISB
            0xD69F03E0, // ERET
        };
        constexpr std::array<u32, 5> tlb_only_eret = {
            0xD5033A9F, // DSB ISHST
            0xD508871F, // TLBI VMALLE1
            0xD5033B9F, // DSB ISH
            0xD5033FDF, // ISB
            0xD69F03E0, // ERET
        };
        std::memcpy(m_kernel_code_host + KernelTlbOnlyEretOffset, tlb_only_eret.data(), sizeof(tlb_only_eret));
        const u32 eret = 0xD69F03E0;
        std::memcpy(m_kernel_code_host + KernelTlbiEretOffset, tlbi_eret.data(),
                    sizeof(tlbi_eret));
        std::memcpy(m_kernel_code_host + KernelEretOffset, &eret, sizeof(eret));

        m_kernel_code_ipa = GetVmState().AllocateIpa(HostAllocationGranule);
        const hv_return_t kernel_map_result =
            hv_vm_map(m_kernel_code_host, m_kernel_code_ipa, HostAllocationGranule,
                      static_cast<hv_memory_flags_t>(HV_MEMORY_READ | HV_MEMORY_EXEC));
        if (kernel_map_result != HV_SUCCESS) {
            GetVmState().FreeIpa(m_kernel_code_ipa, HostAllocationGranule);
            m_kernel_code_ipa = 0;
            ThrowHvError("hv_vm_map(exception vector)", kernel_map_result);
        }
        m_kernel_code_mapped = true;

        // TTBR1 covers the high 39-bit kernel range. Offset zero corresponds to KernelRegionBase.
        for (u64 offset = 0; offset < KernelCodeSize; offset += GuestPageSize) {
            WritePage(m_kernel_root, offset, m_kernel_code_ipa + offset,
                      PermissionAttributes(Common::MemoryPermission::Read |
                                               Common::MemoryPermission::Execute,
                                           true));
        }
    }

    TableStorage AllocateTableStorage() {
        if (m_table_blocks.empty() || m_table_blocks.back().used_slots == 4) {
            TableBlock block{};
            block.host = static_cast<u8*>(
                mmap(nullptr, HostAllocationGranule, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            if (block.host == MAP_FAILED) {
                throw std::runtime_error(
                    fmt::format("mmap(page-table block) failed: {}", strerror(errno)));
            }
            std::memset(block.host, 0, HostAllocationGranule);
            block.ipa = GetVmState().AllocateIpa(HostAllocationGranule);
            try {
                CheckHv("hv_vm_map(page-table block)",
                        hv_vm_map(block.host, block.ipa, HostAllocationGranule,
                                  static_cast<hv_memory_flags_t>(HV_MEMORY_READ |
                                                                 HV_MEMORY_WRITE)));
            } catch (...) {
                GetVmState().FreeIpa(block.ipa, HostAllocationGranule);
                munmap(block.host, HostAllocationGranule);
                throw;
            }
            m_table_blocks.push_back(block);
        }

        TableBlock& block = m_table_blocks.back();
        const u64 offset = static_cast<u64>(block.used_slots++) * GuestPageSize;
        return {.entries = reinterpret_cast<u64*>(block.host + offset), .ipa = block.ipa + offset};
    }

    Level* NewLevel() {
        auto level = std::make_unique<Level>();
        level->storage = AllocateTableStorage();
        std::fill_n(level->storage.entries, 512, 0ULL);
        Level* ptr = level.get();
        m_levels.push_back(std::move(level));
        return ptr;
    }

    u64* FindPageEntry(Level* root, u64 va, bool create, PageAttributes** attributes = nullptr) {
        Level* level = root;
        for (u32 depth = 0; depth < 3; ++depth) {
            const u32 index = Index(va, depth);
            if (level->next[index]) {
                level = level->next[index];
                continue;
            }
            if (depth == 2 || (!create && (level->storage.entries[index] & 1))) {
                if (attributes) {
                    *attributes = &level->attributes[index];
                }
                return &level->storage.entries[index];
            }
            if (!create) {
                return nullptr;
            }
            level = Split(level, index, depth);
        }
        return nullptr;
    }

    void WritePage(Level* root, u64 va, u64 ipa, u64 attributes) {
        u64* pte = FindPageEntry(root, va, true);
        *pte = (ipa & ~0xfffULL) | 3ULL | attributes;
    }

    void ValidateVirtualRange(u64 va, u64 size) const {
        if ((va & (GuestPageSize - 1)) != 0 || (size & (GuestPageSize - 1)) != 0 ||
            size == 0 || va + size < va || va + size > m_user_address_limit) {
            throw std::runtime_error(fmt::format("Invalid AppleHv guest VA range {:#x}+{:#x}",
                                                 va, size));
        }
    }

    void ValidateGuestRange(u64 va, u64 pa, u64 size) const {
        ValidateVirtualRange(va, size);
        if ((pa & (GuestPageSize - 1)) != 0 || pa < DramBase ||
            pa + size < pa || pa + size - DramBase > m_ram_size) {
            throw std::runtime_error(fmt::format("Invalid AppleHv guest PA range {:#x}+{:#x}",
                                                 pa, size));
        }
    }

    void NotifyMappingChangedLocked() {
        // Publish before the epoch. Structural/restrictive edits hold the execution gate;
        // permission relaxations leave only stale, more restrictive TLB entries. Readers
        // flush their own epoch at EL1; a stale permission fault takes the same retry path.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        m_generation.fetch_add(1, std::memory_order_release);
    }

    void StopVcpusLocked() {
        m_exit_vcpus.clear();
        const bool target_running = (OptimizationFlags() & TargetRunningVcpus) != 0;
        for (const auto& [vcpu, running] : m_active_vcpus) {
            if (!target_running || running->load(std::memory_order_acquire)) {
                m_exit_vcpus.push_back(vcpu);
            }
        }
        if (!m_exit_vcpus.empty()) {
            const hv_return_t result =
                hv_vcpus_exit(m_exit_vcpus.data(), static_cast<u32>(m_exit_vcpus.size()));
            if (result != HV_SUCCESS) {
                LOG_WARNING(Core_ARM, "hv_vcpus_exit before mapping change failed with {:#x}",
                            static_cast<u32>(result));
            }
        }
    }

    void Cleanup() noexcept {
        m_valid = false;

        if (m_kernel_code_mapped) {
            const hv_return_t result = hv_vm_unmap(m_kernel_code_ipa, HostAllocationGranule);
            if (result == HV_SUCCESS) {
                GetVmState().FreeIpa(m_kernel_code_ipa, HostAllocationGranule);
                m_kernel_code_mapped = false;
            } else {
                LOG_ERROR(Core_ARM, "hv_vm_unmap(exception vector) failed with {:#x}",
                          static_cast<u32>(result));
            }
        }
        if (m_kernel_code_host && !m_kernel_code_mapped) {
            munmap(m_kernel_code_host, HostAllocationGranule);
            m_kernel_code_host = nullptr;
        }

        for (auto it = m_table_blocks.rbegin(); it != m_table_blocks.rend(); ++it) {
            const hv_return_t result = hv_vm_unmap(it->ipa, HostAllocationGranule);
            if (result == HV_SUCCESS) {
                GetVmState().FreeIpa(it->ipa, HostAllocationGranule);
                munmap(it->host, HostAllocationGranule);
            } else {
                LOG_ERROR(Core_ARM, "hv_vm_unmap(page-table block) failed with {:#x}",
                          static_cast<u32>(result));
            }
        }
        m_table_blocks.clear();
        m_levels.clear();
        m_user_root = nullptr;
        m_kernel_root = nullptr;

        if (m_ram_mapped) {
            const hv_return_t result = hv_vm_unmap(m_ram_ipa, m_ram_size);
            if (result == HV_SUCCESS) {
                GetVmState().FreeIpa(m_ram_ipa, m_ram_size);
                m_ram_mapped = false;
            } else {
                LOG_ERROR(Core_ARM, "hv_vm_unmap(DeviceMemory) failed with {:#x}",
                          static_cast<u32>(result));
            }
        }

        if (m_vm_acquired) {
            GetVmState().Release();
            m_vm_acquired = false;
        }
    }

private:
    inline static std::atomic<u64> s_next_identity{1};
    const u64 m_identity{s_next_identity.fetch_add(1)};
    mutable std::mutex m_mutex;
    std::shared_mutex m_execution_mutex;
    std::atomic<u64> m_generation{1};
    std::atomic<u64> m_code_generation{1};
    std::vector<std::pair<hv_vcpu_t, std::atomic_bool*>> m_active_vcpus;
    std::vector<hv_vcpu_t> m_exit_vcpus;

    bool m_valid{};
    bool m_vm_acquired{};

    u8* m_ram_host{};
    u64 m_ram_size{};
    u64 m_ram_ipa{};
    bool m_ram_mapped{};
    const u32 m_address_space_bits;
    const u64 m_user_address_limit;

    std::vector<TableBlock> m_table_blocks;
    std::vector<std::unique_ptr<Level>> m_levels;
    Level* m_user_root{};
    Level* m_kernel_root{};

    u8* m_kernel_code_host{};
    u64 m_kernel_code_ipa{};
    bool m_kernel_code_mapped{};
};

AddressSpace::AddressSpace(IDeviceMemory& device_memory, IKProcessPageTable& page_table, u32 address_space_bits)
    : m_impl{std::make_unique<Impl>(device_memory, address_space_bits)}, m_page_table{&page_table} {
    page_table.SetNativeMemoryObserver(this);
}

AddressSpace::~AddressSpace() {
    DetachPageTable();
}

void AddressSpace::DetachPageTable() {
    if (m_page_table) {
        m_page_table->SetNativeMemoryObserver(nullptr);
        m_page_table = nullptr;
    }
}

std::shared_ptr<void> AddressSpace::RetainVm() {
    auto& vm = GetVmState();
    vm.Acquire();
    return std::shared_ptr<void>(&vm, [](void* pointer) {
        static_cast<VmState*>(pointer)->Release();
    });
}

u64 AddressSpace::Identity() const noexcept {
    return m_impl->Identity();
}

bool AddressSpace::IsValid() const noexcept {
    return m_impl && m_impl->IsValid();
}

u64 AddressSpace::CodeGeneration() const noexcept { return m_impl->CodeGeneration(); }
u64 AddressSpace::KernelTlbOnlyEretAddress() const noexcept { return m_impl->KernelTlbOnlyEretAddress(); }

u64 AddressSpace::Generation() const noexcept {
    return m_impl->Generation();
}

u64 AddressSpace::KernelEretAddress() const noexcept {
    return m_impl->KernelEretAddress();
}

u64 AddressSpace::KernelTlbiEretAddress() const noexcept {
    return m_impl->KernelTlbiEretAddress();
}

bool AddressSpace::InitializeVcpu(u64 vcpu) const {
    return m_impl->InitializeVcpu(static_cast<hv_vcpu_t>(vcpu));
}

std::shared_lock<std::shared_mutex> AddressSpace::LockExecution(std::atomic_bool& running) {
    return m_impl->LockExecution(running);
}

void AddressSpace::LogFault(u64 va) { m_impl->LogFault(va); }

bool AddressSpace::AllowsAccess(u64 address, Common::MemoryPermission access) {
    return m_impl->AllowsAccess(address, access);
}

void AddressSpace::InvalidateCode() {
    m_impl->InvalidateCode();
}

void AddressSpace::RegisterVcpu(u64 vcpu, std::atomic_bool& running) {
    m_impl->RegisterVcpu(static_cast<hv_vcpu_t>(vcpu), running);
}

void AddressSpace::UnregisterVcpu(u64 vcpu) {
    m_impl->UnregisterVcpu(static_cast<hv_vcpu_t>(vcpu));
}

u64 AddressSpace::MappingGranule(u64 va) {
    return m_impl->MappingGranule(va);
}

void AddressSpace::Map(Common::ProcessAddress virtual_address,
                       Common::PhysicalAddress physical_address, u64 size,
                       Common::MemoryPermission permissions) {
    m_impl->Map(virtual_address.GetValue(), physical_address.GetValue(), size, permissions);
}

void AddressSpace::Track(Common::ProcessAddress virtual_address, u64 size,
                         Common::MemoryPermission permissions) {
    m_impl->Track(virtual_address.GetValue(), size, permissions);
}

void AddressSpace::TrackBatch(std::span<const Common::NativeMemoryTrackRange> ranges) {
    m_impl->TrackBatch(ranges);
}

void AddressSpace::Unmap(Common::ProcessAddress virtual_address, u64 size) {
    m_impl->Unmap(virtual_address.GetValue(), size);
}

void AddressSpace::Protect(Common::ProcessAddress virtual_address, u64 size,
                           Common::MemoryPermission permissions) {
    m_impl->Protect(virtual_address.GetValue(), size, permissions);
}

} // namespace Core::AppleHv
