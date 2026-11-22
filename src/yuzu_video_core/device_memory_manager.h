// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <set>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "yuzu_common/nvdata.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/range_mutex.h"
#include "yuzu_common/scratch_buffer.h"
#include "yuzu_common/virtual_buffer.h"
#include "yuzu_video_core/flush_region_cache_filter.h"

#include <nxemu-module-spec/cpu.h>
#include <nxemu-module-spec/operating_system.h>

namespace Core {

constexpr size_t DEVICE_PAGEBITS = 12ULL;
constexpr size_t DEVICE_PAGESIZE = 1ULL << DEVICE_PAGEBITS;
constexpr size_t DEVICE_PAGEMASK = DEVICE_PAGESIZE - 1ULL;

class DeviceMemory;

template <typename DTraits>
struct DeviceMemoryManagerAllocator;

template <typename Traits>
class DeviceMemoryManager {
    using DeviceInterface = typename Traits::DeviceInterface;
    using DeviceMethods = typename Traits::DeviceMethods;

public:
    DeviceMemoryManager(const IDeviceMemory & device_memory);
    ~DeviceMemoryManager();

    void BindInterface(DeviceInterface* device_inter);

    DAddr Allocate(size_t size);
    void AllocateFixed(DAddr start, size_t size);
    void Free(DAddr start, size_t size);

    void Map(DAddr address, VAddr virtual_address, size_t size, Asid asid, bool track = false);

    void Unmap(DAddr address, size_t size);

    // Kept for the Host1x interface. Map derives continuity from the physical pages
    // it installs, including heap mappings that do not request pretracking.
    void TrackContinuity(DAddr, VAddr, size_t, Asid) {}

    // Write / Read
    template <typename T>
    T* GetPointer(DAddr address);

    template <typename T>
    const T* GetPointer(DAddr address) const;

    template <typename Func>
    void ApplyOpOnPAddr(PAddr address, Common::ScratchBuffer<u32>& buffer, Func&& operation) {
        DAddr subbits = static_cast<DAddr>(address & page_mask);
        const u32 base = compressed_device_addr[(address >> page_bits)];
        if ((base >> MULTI_FLAG_BITS) == 0) [[likely]] {
            const DAddr d_address = (static_cast<DAddr>(base) << page_bits) + subbits;
            operation(d_address);
            return;
        }
        InnerGatherDeviceAddresses(buffer, address);
        for (u32 value : buffer) {
            operation((static_cast<DAddr>(value) << page_bits) + subbits);
        }
    }

    template <typename Func>
    void ApplyOpOnPointer(const u8* p, Common::ScratchBuffer<u32>& buffer, Func&& operation) {
        PAddr address = GetRawPhysicalAddr<u8>(p);
        ApplyOpOnPAddr(address, buffer, operation);
    }

    PAddr GetPhysicalRawAddressFromDAddr(DAddr address) const {
        PAddr subbits = static_cast<PAddr>(address & page_mask);
        auto paddr = compressed_physical_ptr[(address >> page_bits)];
        if (paddr == 0) {
            return 0;
        }
        return (static_cast<PAddr>(paddr - 1) << page_bits) + subbits;
    }

    template <typename T>
    void Write(DAddr address, T value);

    template <typename T>
    T Read(DAddr address) const;

    u8* GetSpan(const DAddr src_addr, const std::size_t size);
    const u8* GetSpan(const DAddr src_addr, const std::size_t size) const;

    void ReadBlock(DAddr address, void* dest_pointer, size_t size);
    void ReadBlockUnsafe(DAddr address, void* dest_pointer, size_t size);
    void WriteBlock(DAddr address, const void* src_pointer, size_t size);
    void WriteBlockUnsafe(DAddr address, const void* src_pointer, size_t size);
    void WriteBlockForGpuReadback(DAddr address, const void* src_pointer, size_t size);

    Asid RegisterProcess(IMemory * memory);
    void UnregisterProcess(Asid id);

    void UpdatePagesCachedCount(DAddr addr, size_t size, s32 delta);
    void UpdatePagesReadCachedCount(DAddr addr, size_t size, s32 delta);
    void UpdatePagesWriteProtectedCount(DAddr addr, uint64_t size, s32 delta);
    // Pages holding bytes the GPU wrote stay cached and write protected, even once a CPU write
    // untracked them: every later CPU write there is reported exactly, never as a coarse write.
    void UpdatePagesGpuModifiedCount(DAddr addr, uint64_t size, s32 delta) {
        UpdatePagesCachedCount(addr, size, delta);
        UpdatePagesWriteProtectedCount(addr, size, delta);
    }
    // Append the ranges of pending query results the GPU already copied into its buffers.
    void GpuWrittenResults(DAddr address, size_t size, std::vector<std::pair<DAddr, u64>>& ranges);
    // Whether read-cached pages fault on CPU access in every registered process.
    bool ProtectsReadCachedPages();
    bool ProtectsQueryResults();
    // Called on the serialized GPU producer, under its execution lock.
    void RegisterNativeRead(DAddr addr, size_t size);
    void InvalidateNativeReadRegion(DAddr addr, size_t size);

    // Presence remains conservative across deletion, remapping and address reuse.
    void RegisterFlushCacheRegion(DAddr addr, u64 size, VideoCommon::CacheType types) {
        flush_cache_filter.Register(addr, size, types);
    }

    [[nodiscard]] VideoCommon::CacheType FilterFlushCaches(
        DAddr addr, u64 size, VideoCommon::CacheType requested) const {
        return flush_cache_filter.Filter(addr, size, requested);
    }

    static constexpr size_t AS_BITS = Traits::device_virtual_bits;

    const u8* PhysicalBase() const noexcept {
        return reinterpret_cast<const u8*>(physical_base);
    }
    uint64_t PhysicalSize() const noexcept {
        return static_cast<uint64_t>(compressed_device_addr.size()) << page_bits;
    }

    void EnableInPlaceReads() {
        if (InPlaceReadsEnabled()) {
            return;
        }
        excluded_pages.resize((device_as_size >> page_bits) / 64);
        in_place_ticks.resize_lazy(device_as_size >> page_bits);
        in_place_pages.resize((device_as_size >> page_bits) / 64);
        in_place_enabled.store(true, std::memory_order_release);
    }
    bool InPlaceReadsEnabled() const noexcept {
        return in_place_enabled.load(std::memory_order_acquire);
    }

    void ExcludeFromInPlace(DAddr addr, uint64_t size) noexcept {
        ForEachPage(addr, size, [this](uint64_t page) { SetPageBit(excluded_pages, page); });
    }
    bool IsExcludedFromInPlace(DAddr addr, uint64_t size) const noexcept {
        if (size != 0 && addr >= device_as_size) {
            return true;
        }
        bool written = false;
        ForEachPage(addr, size,
                    [&](uint64_t page) { written = written || TestPageBit(excluded_pages, page); });
        return written;
    }

    void MarkEngineWritten(DAddr addr, uint64_t size) noexcept {
        ForEachPage(addr, size, [this](uint64_t page) { SetPageBit(engine_pages, page); });
    }
    bool IsEngineWritten(DAddr addr, uint64_t size) const noexcept {
        if (size != 0 && addr >= device_as_size) {
            return true;
        }
        bool written = false;
        ForEachPage(addr, size,
                    [&](uint64_t page) { written = written || TestPageBit(engine_pages, page); });
        return written;
    }
    void PrepareEngineWrite(DAddr addr, uint64_t size);

    void HoldInPlaceRead(DAddr addr, uint64_t size, u64 tick) {
        ForEachPage(addr, size, [&](uint64_t page) {
            std::atomic_ref<u64>{in_place_ticks[page]}.store(tick, std::memory_order_relaxed);
        });
        // Pairs with the fence in ReleaseInPlaceReads: a release either sees this tick, or this
        // sees the release and protects the page again after it
        std::atomic_thread_fence(std::memory_order_seq_cst);
        bool unprotected = in_place_releasing.load(std::memory_order_relaxed);
        ForEachPage(addr, size, [&](uint64_t page) {
            unprotected = unprotected || !TestPageBit(in_place_pages, page);
        });
        if (!unprotected) [[likely]] {
            return;
        }
        std::scoped_lock lock{in_place_mutex};
        ForEachPage(addr, size, [this](uint64_t page) {
            if (TestPageBit(in_place_pages, page)) {
                return;
            }
            SetPageBit(in_place_pages, page);
            TrapCpuWrites(page);
        });
    }

    template <typename Wait>
    void ReleaseInPlaceReads(DAddr addr, uint64_t size, Wait&& wait) {
        if (!InPlaceReadsEnabled()) {
            return;
        }
        ForEachPage(addr, size, [&](uint64_t page) {
            while (TestPageBit(in_place_pages, page)) {
                const u64 tick =
                    std::atomic_ref<u64>{in_place_ticks[page]}.load(std::memory_order_acquire);
                wait(tick);
                std::scoped_lock lock{in_place_mutex};
                if (!TestPageBit(in_place_pages, page)) {
                    continue;
                }
                in_place_releasing.store(true, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                const u64 latest_tick =
                    std::atomic_ref<u64>{in_place_ticks[page]}.load(std::memory_order_relaxed);
                if (latest_tick == tick) {
                    ClearPageBit(in_place_pages, page);
                    UntrapCpuWrites(page);
                    SetPageBit(excluded_pages, page);
                }
                in_place_releasing.store(false, std::memory_order_release);
            }
        });
    }

private:
    static constexpr size_t device_virtual_bits = Traits::device_virtual_bits;

    /// Calls func(memory, vaddr, size) for each run of the range backed by one process
    template <typename Func>
    void ForEachProcessRange(DAddr addr, uint64_t size, Func&& func);
    template <typename Func>
    void ForRangeAndAliases(DAddr addr, uint64_t size, Func&& func);
    void MarkEngineWrittenAliases(DAddr addr, uint64_t size);
    bool HasAliases(uint64_t page) const noexcept {
        const u32 physical = compressed_physical_ptr[page];
        return physical != 0 && (compressed_device_addr[physical - 1U] >> MULTI_FLAG_BITS) != 0;
    }
    bool NeedsNoInPlacePreparation(uint64_t page) const noexcept {
        return !InPlaceReadsEnabled() ||
               (TestPageBit(excluded_pages, page) && !TestPageBit(in_place_pages, page));
    }
    static constexpr size_t device_as_size = 1ULL << device_virtual_bits;
    static constexpr size_t page_bits = 12;
    static constexpr size_t page_size = 1ULL << page_bits;
    static constexpr size_t page_mask = page_size - 1ULL;
    static constexpr u32 physical_address_base = 1U << page_bits;
    static constexpr u32 MULTI_FLAG_BITS = 31;
    static constexpr u32 MULTI_FLAG = 1U << MULTI_FLAG_BITS;
    static constexpr u32 MULTI_MASK = ~MULTI_FLAG;

    template <typename T>
    T* GetPointerFromRaw(PAddr addr) {
        return reinterpret_cast<T*>(physical_base + addr);
    }

    template <typename T>
    const T* GetPointerFromRaw(PAddr addr) const {
        return reinterpret_cast<T*>(physical_base + addr);
    }

    template <typename T>
    PAddr GetRawPhysicalAddr(const T* ptr) const {
        return static_cast<PAddr>(reinterpret_cast<uintptr_t>(ptr) - physical_base);
    }

    // A set bit marks a change of physical run (including mapped/unmapped edges).
    // Higher levels mark nonempty words below, so updates and searches are bounded
    // by the address-space depth rather than the length of an adjacent run.
    void SetContinuityBoundary(size_t page, bool boundary);
    void UpdateContinuityBoundary(size_t page);
    size_t NextContinuityBoundary(size_t first_page, size_t end_page) const;

    void WalkBlock(const DAddr addr, const std::size_t size, auto on_unmapped, auto on_memory,
                   auto increment);

    void InnerGatherDeviceAddresses(Common::ScratchBuffer<u32>& buffer, PAddr address);

    template <typename Func>
    static void ForEachPage(DAddr addr, uint64_t size, Func&& func) {
        if (size == 0 || addr >= device_as_size) {
            return;
        }
        const uint64_t last_page = std::min<uint64_t>(addr + size - 1, device_as_size - 1) >> page_bits;
        for (uint64_t page = addr >> page_bits; page <= last_page; ++page) {
            func(page);
        }
    }
    void TrapCpuWrites(uint64_t page) {
        UpdatePagesCachedCount(page << page_bits, page_size, 1);
        UpdatePagesWriteProtectedCount(page << page_bits, page_size, 1);
    }
    void UntrapCpuWrites(uint64_t page) {
        UpdatePagesWriteProtectedCount(page << page_bits, page_size, -1);
        UpdatePagesCachedCount(page << page_bits, page_size, -1);
    }
    static void SetPageBit(Common::VirtualBuffer<u64>& bits, uint64_t page) noexcept {
        std::atomic_ref<u64> word{bits[page >> 6]};
        const u64 bit = u64{1} << (page & 63);
        if ((word.load(std::memory_order_relaxed) & bit) == 0) {
            word.fetch_or(bit, std::memory_order_release);
        }
    }
    static void ClearPageBit(Common::VirtualBuffer<u64>& bits, uint64_t page) noexcept {
        std::atomic_ref<u64>{bits[page >> 6]}.fetch_and(~(u64{1} << (page & 63)),
                                                        std::memory_order_release);
    }
    static bool TestPageBit(const Common::VirtualBuffer<u64>& bits, uint64_t page) noexcept {
        const std::atomic_ref<u64> word{const_cast<u64&>(bits[page >> 6])};
        return (word.load(std::memory_order_acquire) & (u64{1} << (page & 63))) != 0;
    }

    std::unique_ptr<DeviceMemoryManagerAllocator<Traits>> impl;

    const uintptr_t physical_base;
    DeviceInterface* device_inter;
    Common::VirtualBuffer<u32> compressed_physical_ptr;
    Common::VirtualBuffer<u32> compressed_device_addr;
    static constexpr size_t continuity_levels = (device_virtual_bits - page_bits + 5) / 6;
    std::array<Common::VirtualBuffer<u64>, continuity_levels> continuity_boundaries;
    Common::VirtualBuffer<u64> excluded_pages;
    Common::VirtualBuffer<u64> engine_pages{(device_as_size >> page_bits) / 64};
    Common::VirtualBuffer<u64> in_place_ticks;
    Common::VirtualBuffer<u64> in_place_pages;
    std::atomic<bool> in_place_enabled{};
    std::mutex in_place_mutex;
    std::atomic<bool> in_place_releasing{};

    // Process memory interfaces

    std::deque<size_t> id_pool;
    std::deque<IMemory*> registered_processes;
    std::mutex process_guard;
    IMemory* GetRegisteredProcess(Asid asid);

    // Memory protection management

    static constexpr size_t guest_max_as_bits = 39;
    static constexpr size_t guest_as_size = 1ULL << guest_max_as_bits;
    static constexpr size_t guest_mask = guest_as_size - 1ULL;
    static constexpr size_t asid_start_bit = guest_max_as_bits;

    std::pair<Asid, VAddr> ExtractCPUBacking(size_t page_index) {
        auto content = cpu_backing_address[page_index];
        const VAddr address = content & guest_mask;
        const Asid asid{static_cast<size_t>(content >> asid_start_bit)};
        return std::make_pair(asid, address);
    }

    void InsertCPUBacking(size_t page_index, VAddr address, Asid asid) {
        cpu_backing_address[page_index] = address | (asid.id << asid_start_bit);
    }

    Common::VirtualBuffer<VAddr> cpu_backing_address;
    using CounterType = u8;
    using CounterAtomicType = std::atomic_uint8_t;
    static constexpr size_t subentries = 8 / sizeof(CounterType);
    static constexpr size_t subentries_mask = subentries - 1;
    static constexpr size_t subentries_shift =
        std::countr_zero(sizeof(u64)) - std::countr_zero(sizeof(CounterType));
    class CounterEntry final {
    public:
        CounterEntry() = default;

        CounterAtomicType& Count(std::size_t page) {
            return values[page & subentries_mask];
        }

        const CounterAtomicType& Count(std::size_t page) const {
            return values[page & subentries_mask];
        }

    private:
        std::array<CounterAtomicType, subentries> values{};
    };
    static_assert(sizeof(CounterEntry) == subentries * sizeof(CounterType),
                  "CounterEntry should be 8 bytes!");

    static constexpr size_t num_counter_entries =
        (1ULL << (device_virtual_bits - page_bits)) / subentries;
    using CachedPages = std::array<CounterEntry, num_counter_entries>;
    std::unique_ptr<CachedPages> cached_pages;
    VideoCommon::FlushRegionCacheFilter<Traits::device_virtual_bits> flush_cache_filter;
    // Physical pages opened by a completed native readback. Only actual GPU image
    // writes consume these observers; cache-management commands leave them alone.
    std::set<size_t> native_read_pages;
    Common::RangeMutex counter_guard;
    std::mutex mapping_guard;
};

} // namespace Core
