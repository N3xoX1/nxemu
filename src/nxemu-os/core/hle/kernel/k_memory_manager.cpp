// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "yuzu_common/alignment.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/initial_process.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_page_group.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_results.h"

namespace Kernel {

namespace {

constexpr KMemoryManager::Pool GetPoolFromMemoryRegionType(u32 type) {
    if ((type | KMemoryRegionType_DramApplicationPool) == type) {
        return KMemoryManager::Pool::Application;
    } else if ((type | KMemoryRegionType_DramAppletPool) == type) {
        return KMemoryManager::Pool::Applet;
    } else if ((type | KMemoryRegionType_DramSystemPool) == type) {
        return KMemoryManager::Pool::System;
    } else if ((type | KMemoryRegionType_DramSystemNonSecurePool) == type) {
        return KMemoryManager::Pool::SystemNonSecure;
    } else {
        UNREACHABLE_MSG("InvalidMemoryRegionType for conversion to Pool");
    }
}

} // namespace

KMemoryManager::KMemoryManager(Core::System& system)
    : m_system{system}, m_memory_layout{system.Kernel().MemoryLayout()},
      m_pool_locks{
          KLightLock{system.Kernel()},
          KLightLock{system.Kernel()},
          KLightLock{system.Kernel()},
          KLightLock{system.Kernel()},
      } {}

void KMemoryManager::CommitBackingPages(KPhysicalAddress address, size_t num_pages) {
    if (num_pages == 0) {
        return;
    }
    auto& buffer = m_system.DeviceMemory().buffer;
    const size_t offset = address.GetValue() - Core::DramMemoryMap::Base;
#ifdef _WIN32
    buffer.CommitBackingRegion(offset, num_pages * PageSize);
#endif
    // Record use only after backing is accessible, including on a later kernel restart.
    buffer.MarkBackingUsed(offset, num_pages * PageSize);
}

void KMemoryManager::ReleaseFreeBacking() {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    static const bool enabled = [] {
        const char* const flag = std::getenv("NXEMU_FREE_RELEASE");
        return flag == nullptr || std::strcmp(flag, "0") != 0;
    }();
    if (!enabled || !m_has_dirty_free_granules.load(std::memory_order_acquire)) {
        return;
    }
    // One pass a second, from whichever thread allocates or frees pages.
    const s64 now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    s64 due = m_next_release_pass_ms.load(std::memory_order_relaxed);
    if (now_ms < due || !m_next_release_pass_ms.compare_exchange_strong(
                            due, now_ms + 1000, std::memory_order_relaxed)) {
        return;
    }
    if (m_release_in_progress.exchange(true, std::memory_order_acquire)) {
        return;
    }
    SCOPE_EXIT {
        m_release_in_progress.store(false, std::memory_order_release);
    };
    // Clear before scanning so a concurrent Close() always arms another pass.
    m_has_dirty_free_granules.store(false, std::memory_order_release);

    // Bound both work and scan length under the pool lock on the calling thread:
    // on Windows/macOS this limits userspace zeroing (<1 ms), and on Linux/Android
    // it caps MADV_REMOVE / fallocate TLB shootdowns across cores (~1-2 ms), while
    // resuming from the saved cursor avoids walking the full 4-8 GiB heap per pass.
    constexpr size_t MaxReleasesPerPass = 128;      // 8 MiB
    constexpr size_t MaxRunGranules = 16;           // 1 MiB per run
    constexpr size_t MaxScanGranulesPerPass = 8192; // 512 MiB of address space
    size_t releases = 0;
    size_t scanned = 0;
    size_t free_dirty = 0;
    bool budget_reached = false;
    auto& buffer = m_system.DeviceMemory().buffer;
    if (m_release_manager_cursor >= m_num_managers) {
        m_release_manager_cursor = 0;
        m_release_granule_cursor = 0;
        m_release_sweep_saw_remaining = false;
    }
    while (m_release_manager_cursor < m_num_managers && !budget_reached) {
        Impl& manager = m_managers[m_release_manager_cursor];
        KPageHeap& heap = manager.GetHeap();
        const size_t heap_offset = heap.GetAddress().GetValue() - Core::DramMemoryMap::Base;
        const size_t pool = static_cast<size_t>(manager.GetPool());
        KScopedLightLock lk(m_pool_locks[pool]);
        if (heap.GetNumFreeGranules() == 0) {
            ++m_release_manager_cursor;
            m_release_granule_cursor = 0;
            continue;
        }
        // An optimized process can reuse its previous page contents without a fill.
        if (m_has_optimized_process[pool]) {
            m_release_sweep_saw_remaining = true;
            ++m_release_manager_cursor;
            m_release_granule_cursor = 0;
            continue;
        }
        const size_t num_granules = heap.GetNumGranules();
        size_t run_begin = 0;
        size_t run_count = 0;
        const auto flush_run = [&] {
            if (run_count == 0) {
                return;
            }
            const size_t run_offset = heap_offset + (run_begin << KPageHeap::GranuleShift);
            const size_t run_length = run_count << KPageHeap::GranuleShift;
            if (buffer.ReleaseFreeBackingRange(run_offset, run_length)) {
                releases += run_count;
                for (size_t g = run_begin; g < run_begin + run_count; ++g) {
                    heap.GranuleReleaseState(g) = 0;
                }
            }
            run_count = 0;
        };
        size_t granule = m_release_granule_cursor;
        for (; granule < num_granules; ++granule) {
            if (scanned >= MaxScanGranulesPerPass || releases >= MaxReleasesPerPass) {
                budget_reached = true;
                break;
            }
            ++scanned;
            u8& seen_free = heap.GranuleReleaseState(granule);
            const size_t offset = heap_offset + (granule << KPageHeap::GranuleShift);
            if (!heap.IsGranuleFree(granule) || !buffer.IsBackingDirty(offset) ||
                buffer.IsBackingPinned(offset)) {
                // In use, already zero, or pinned for GPU import: keep its backing page intact.
                flush_run();
                seen_free = 0;
                continue;
            }
            ++free_dirty;
            if (seen_free == 0) {
                flush_run();
                seen_free = 1;
                m_release_sweep_saw_remaining = true;
                continue;
            }
            if (run_count == 0) {
                run_begin = granule;
            }
            ++run_count;
            if (run_count == MaxRunGranules || releases + run_count == MaxReleasesPerPass) {
                flush_run();
            }
        }
        flush_run();
        if (granule < num_granules) {
            m_release_granule_cursor = granule;
            budget_reached = true;
        } else {
            ++m_release_manager_cursor;
            m_release_granule_cursor = 0;
        }
    }
    if (free_dirty > releases) {
        m_release_sweep_saw_remaining = true;
    }
    if (m_release_manager_cursor < m_num_managers) {
        m_has_dirty_free_granules.store(true, std::memory_order_release);
    } else {
        if (m_release_sweep_saw_remaining) {
            m_has_dirty_free_granules.store(true, std::memory_order_release);
        }
        m_release_manager_cursor = 0;
        m_release_granule_cursor = 0;
        m_release_sweep_saw_remaining = false;
    }
    const u64 total =
        m_released_granules_total.fetch_add(releases, std::memory_order_relaxed) + releases;
    if (now_ms >= m_next_release_log_ms.load(std::memory_order_relaxed)) {
        m_next_release_log_ms.store(now_ms + 30000, std::memory_order_relaxed);
        LOG_INFO(Kernel, "Free guest memory released: {} MiB total, {} MiB free and not yet zero",
                 (total << KPageHeap::GranuleShift) >> 20,
                 ((free_dirty - releases) << KPageHeap::GranuleShift) >> 20);
    }
#endif
}

void KMemoryManager::MarkBackingPagesDirty(KPhysicalAddress address, size_t num_pages) {
    // Freed pages may hold data: the next allocation that clears them must really clear them.
    m_system.DeviceMemory().buffer.MarkBackingDirty(address.GetValue() - Core::DramMemoryMap::Base,
                                                    num_pages * PageSize);
    m_has_dirty_free_granules.store(true, std::memory_order_release);
}

void KMemoryManager::Initialize(KVirtualAddress management_region, size_t management_region_size) {
    // A kernel started again over the same backing memory finds the previous run's data.
    m_system.DeviceMemory().buffer.ForgetBackingContents();
    m_has_dirty_free_granules.store(true, std::memory_order_release);
    m_release_manager_cursor = 0;
    m_release_granule_cursor = 0;
    m_release_sweep_saw_remaining = false;

    // Clear the management region to zero.
    const KVirtualAddress management_region_end = management_region + management_region_size;
    // std::memset(GetVoidPointer(management_region), 0, management_region_size);

    // Reset our manager count.
    m_num_managers = 0;

    // Traverse the virtual memory layout tree, initializing each manager as appropriate.
    while (m_num_managers != MaxManagerCount) {
        // Locate the region that should initialize the current manager.
        KPhysicalAddress region_address = 0;
        size_t region_size = 0;
        Pool region_pool = Pool::Count;
        for (const auto& it : m_system.Kernel().MemoryLayout().GetPhysicalMemoryRegionTree()) {
            // We only care about regions that we need to create managers for.
            if (!it.IsDerivedFrom(KMemoryRegionType_DramUserPool)) {
                continue;
            }

            // We want to initialize the managers in order.
            if (it.GetAttributes() != m_num_managers) {
                continue;
            }

            const KPhysicalAddress cur_start = it.GetAddress();
            const KPhysicalAddress cur_end = it.GetEndAddress();

            // Validate the region.
            ASSERT(cur_end != 0);
            ASSERT(cur_start != 0);
            ASSERT(it.GetSize() > 0);

            // Update the region's extents.
            if (region_address == 0) {
                region_address = cur_start;
                region_size = it.GetSize();
                region_pool = GetPoolFromMemoryRegionType(it.GetType());
            } else {
                ASSERT(cur_start == region_address + region_size);

                // Update the size.
                region_size = cur_end - region_address;
                ASSERT(GetPoolFromMemoryRegionType(it.GetType()) == region_pool);
            }
        }

        // If we didn't find a region, we're done.
        if (region_size == 0) {
            break;
        }

        // Initialize a new manager for the region.
        Impl* manager = std::addressof(m_managers[m_num_managers++]);
        ASSERT(m_num_managers <= m_managers.size());

        const size_t cur_size = manager->Initialize(region_address, region_size, management_region,
                                                    management_region_end, region_pool);
        management_region += cur_size;
        ASSERT(management_region <= management_region_end);

        // Insert the manager into the pool list.
        const auto region_pool_index = static_cast<u32>(region_pool);
        if (m_pool_managers_tail[region_pool_index] == nullptr) {
            m_pool_managers_head[region_pool_index] = manager;
        } else {
            m_pool_managers_tail[region_pool_index]->SetNext(manager);
            manager->SetPrev(m_pool_managers_tail[region_pool_index]);
        }
        m_pool_managers_tail[region_pool_index] = manager;
    }

    // Free each region to its corresponding heap.
    size_t reserved_sizes[MaxManagerCount] = {};
    const KPhysicalAddress ini_start = GetInitialProcessBinaryPhysicalAddress();
    const size_t ini_size = GetInitialProcessBinarySize();
    const KPhysicalAddress ini_end = ini_start + ini_size;
    const KPhysicalAddress ini_last = ini_end - 1;
    for (const auto& it : m_system.Kernel().MemoryLayout().GetPhysicalMemoryRegionTree()) {
        if (it.IsDerivedFrom(KMemoryRegionType_DramUserPool)) {
            // Get the manager for the region.
            auto& manager = m_managers[it.GetAttributes()];

            const KPhysicalAddress cur_start = it.GetAddress();
            const KPhysicalAddress cur_last = it.GetLastAddress();
            const KPhysicalAddress cur_end = it.GetEndAddress();

            if (cur_start <= ini_start && ini_last <= cur_last) {
                // Free memory before the ini to the heap.
                if (cur_start != ini_start) {
                    manager.Free(cur_start, (ini_start - cur_start) / PageSize);
                }

                // Open/reserve the ini memory.
                CommitBackingPages(ini_start, ini_size / PageSize);
                manager.OpenFirst(ini_start, ini_size / PageSize);
                reserved_sizes[it.GetAttributes()] += ini_size;

                // Free memory after the ini to the heap.
                if (ini_last != cur_last) {
                    ASSERT(cur_end != 0);
                    manager.Free(ini_end, (cur_end - ini_end) / PageSize);
                }
            } else {
                // Ensure there's no partial overlap with the ini image.
                if (cur_start <= ini_last) {
                    ASSERT(cur_last < ini_start);
                } else {
                    // Otherwise, check the region for general validity.
                    ASSERT(cur_end != 0);
                }

                // Free the memory to the heap.
                manager.Free(cur_start, it.GetSize() / PageSize);
            }
        }
    }

    // Update the used size for all managers.
    for (size_t i = 0; i < m_num_managers; ++i) {
        m_managers[i].SetInitialUsedHeapSize(reserved_sizes[i]);
    }
}

Result KMemoryManager::InitializeOptimizedMemory(u64 process_id, Pool pool) {
    const u32 pool_index = static_cast<u32>(pool);

    // Lock the pool.
    KScopedLightLock lk(m_pool_locks[pool_index]);

    // Check that we don't already have an optimized process.
    R_UNLESS(!m_has_optimized_process[pool_index], ResultBusy);

    // Clear the management area for the optimized process before publishing it.
    try {
        for (auto* manager = this->GetFirstManager(pool, Direction::FromFront); manager != nullptr;
             manager = this->GetNextManager(manager, Direction::FromFront)) {
            manager->InitializeOptimizedMemory(m_system.Kernel());
        }
    } catch (const std::bad_alloc&) {
        R_THROW(ResultOutOfMemory);
    }

    // Set the optimized process id.
    m_optimized_process_ids[pool_index] = process_id;
    m_has_optimized_process[pool_index] = true;

    R_SUCCEED();
}

void KMemoryManager::FinalizeOptimizedMemory(u64 process_id, Pool pool) {
    const u32 pool_index = static_cast<u32>(pool);

    // Lock the pool.
    KScopedLightLock lk(m_pool_locks[pool_index]);

    // If the process was optimized, clear it.
    if (m_has_optimized_process[pool_index] && m_optimized_process_ids[pool_index] == process_id) {
        m_has_optimized_process[pool_index] = false;
        m_has_dirty_free_granules.store(true, std::memory_order_release);
    }
}

size_t KMemoryManager::GuestPagesPerHostPage() {
#ifdef _WIN32
    return 1;
#else
    static const size_t ratio =
        std::max<size_t>(1, static_cast<size_t>(sysconf(_SC_PAGESIZE)) / PageSize);
    return ratio;
#endif
}

KPhysicalAddress KMemoryManager::AllocateAndOpenContinuous(size_t num_pages, size_t align_pages,
                                                           u32 option) {
    // Early return if we're allocating no pages.
    if (num_pages == 0) {
        return 0;
    }

    KPhysicalAddress allocated_block = 0;
    {
        // Lock the pool that we're allocating from.
        const auto [pool, dir] = DecodeOption(option);
        KScopedLightLock lk(m_pool_locks[static_cast<std::size_t>(pool)]);

        // Choose a heap based on our page size request.
        const s32 heap_index = KPageHeap::GetAlignedBlockIndex(num_pages, align_pages);
        if (heap_index < 0) {
            return 0;
        }

        // Loop, trying to iterate from each block.
        Impl* chosen_manager = nullptr;
        for (chosen_manager = this->GetFirstManager(pool, dir); chosen_manager != nullptr;
             chosen_manager = this->GetNextManager(chosen_manager, dir)) {
            allocated_block = chosen_manager->AllocateAligned(heap_index, num_pages, align_pages);
            if (allocated_block != 0) {
                break;
            }
        }

        // If we failed to allocate, quit now.
        if (allocated_block == 0) {
            return 0;
        }

        // Commit physical backing before the kernel or an alias can touch the new pages.
        try {
            CommitBackingPages(allocated_block, num_pages);
        } catch (const std::bad_alloc&) {
            chosen_manager->Free(allocated_block, num_pages);
            return 0;
        }

        // Maintain the optimized memory bitmap, if we should.
        if (m_has_optimized_process[static_cast<size_t>(pool)]) {
            chosen_manager->TrackUnoptimizedAllocation(m_system.Kernel(), allocated_block,
                                                       num_pages);
        }

        // Open the first reference to the pages.
        chosen_manager->OpenFirst(allocated_block, num_pages);
    }

    ReleaseFreeBacking();
    return allocated_block;
}

Result KMemoryManager::AllocatePageGroupImpl(KPageGroup* out, size_t num_pages, Pool pool,
                                             Direction dir, bool unoptimized, bool random) {
    // Choose a heap based on our page size request.
    const s32 heap_index = KPageHeap::GetBlockIndex(num_pages);
    R_UNLESS(0 <= heap_index, ResultOutOfMemory);

    // Ensure that we don't leave anything un-freed.
    ON_RESULT_FAILURE {
        for (const auto& it : *out) {
            // KPageGroup can concatenate physically adjacent blocks from different managers.
            auto address = it.GetAddress();
            size_t remaining = it.GetNumPages();
            while (remaining > 0) {
                auto& manager = this->GetManager(address);
                const size_t pages = std::min(remaining, manager.GetPageOffsetToEnd(address));
                manager.Free(address, pages);
                remaining -= pages;
                address += pages * PageSize;
            }
        }
        out->Finalize();
    };

    // Keep allocating until we've allocated all our pages.
    for (s32 index = heap_index; index >= 0 && num_pages > 0; index--) {
        const size_t pages_per_alloc = KPageHeap::GetBlockNumPages(index);
        for (Impl* cur_manager = this->GetFirstManager(pool, dir); cur_manager != nullptr;
             cur_manager = this->GetNextManager(cur_manager, dir)) {
            while (num_pages >= pages_per_alloc) {
                // Allocate a block.
                KPhysicalAddress allocated_block = cur_manager->AllocateBlock(index, random);
                if (allocated_block == 0) {
                    break;
                }

                // Ensure we don't leak the block if we fail.
                ON_RESULT_FAILURE_2 {
                    cur_manager->Free(allocated_block, pages_per_alloc);
                };

                // Add the block to our group.
                R_TRY(out->AddBlock(allocated_block, pages_per_alloc));

                // Maintain the optimized memory bitmap, if we should.
                if (unoptimized) {
                    cur_manager->TrackUnoptimizedAllocation(m_system.Kernel(), allocated_block,
                                                            pages_per_alloc);
                }

                num_pages -= pages_per_alloc;
            }
        }
    }

    // Only succeed if we allocated as many pages as we wanted.
    R_UNLESS(num_pages == 0, ResultOutOfMemory);
    try {
        for (const auto& block : *out) {
            CommitBackingPages(block.GetAddress(), block.GetNumPages());
        }
    } catch (const std::bad_alloc&) {
        // Use the same rollback as guest heap exhaustion before opening any references.
        R_THROW(ResultOutOfMemory);
    }

    // We succeeded!
    R_SUCCEED();
}

Result KMemoryManager::AllocateAndOpen(KPageGroup* out, size_t num_pages, u32 option) {
    ASSERT(out != nullptr);
    ASSERT(out->GetNumPages() == 0);

    // Early return if we're allocating no pages.
    R_SUCCEED_IF(num_pages == 0);

    {
        // Lock the pool that we're allocating from.
        const auto [pool, dir] = DecodeOption(option);
        KScopedLightLock lk(m_pool_locks[static_cast<size_t>(pool)]);

        // Allocate the page group.
        R_TRY(this->AllocatePageGroupImpl(out, num_pages, pool, dir,
                                          m_has_optimized_process[static_cast<size_t>(pool)], true));

        // Open the first reference to the pages.
        for (const auto& block : *out) {
            KPhysicalAddress cur_address = block.GetAddress();
            size_t remaining_pages = block.GetNumPages();
            while (remaining_pages > 0) {
                // Get the manager for the current address.
                auto& manager = this->GetManager(cur_address);

                // Process part or all of the block.
                const size_t cur_pages =
                    std::min(remaining_pages, manager.GetPageOffsetToEnd(cur_address));
                manager.OpenFirst(cur_address, cur_pages);

                // Advance.
                cur_address += cur_pages * PageSize;
                remaining_pages -= cur_pages;
            }
        }
    }

    ReleaseFreeBacking();
    R_SUCCEED();
}

Result KMemoryManager::AllocateForProcess(KPageGroup* out, size_t num_pages, u32 option,
                                          u64 process_id, u8 fill_pattern) {
    ASSERT(out != nullptr);
    ASSERT(out->GetNumPages() == 0);

    // Decode the option.
    const auto [pool, dir] = DecodeOption(option);

    // Allocate the memory.
    bool optimized;
    {
        // Lock the pool that we're allocating from.
        KScopedLightLock lk(m_pool_locks[static_cast<size_t>(pool)]);

        // Check if we have an optimized process.
        const bool has_optimized = m_has_optimized_process[static_cast<size_t>(pool)];
        const bool is_optimized = m_optimized_process_ids[static_cast<size_t>(pool)] == process_id;

        // Allocate the page group.
        R_TRY(this->AllocatePageGroupImpl(out, num_pages, pool, dir, has_optimized && !is_optimized,
                                          false));

        // Set whether we should optimize.
        optimized = has_optimized && is_optimized;
    }

    // Perform optimized memory tracking, if we should.
    if (optimized) {
        // Iterate over the allocated blocks.
        for (const auto& block : *out) {
            // Get the block extents.
            const KPhysicalAddress block_address = block.GetAddress();
            const size_t block_pages = block.GetNumPages();

            // If it has no pages, we don't need to do anything.
            if (block_pages == 0) {
                continue;
            }

            // Fill all the pages that we need to fill.
            bool any_new = false;
            {
                KPhysicalAddress cur_address = block_address;
                size_t remaining_pages = block_pages;
                while (remaining_pages > 0) {
                    // Get the manager for the current address.
                    auto& manager = this->GetManager(cur_address);

                    // Process part or all of the block.
                    const size_t cur_pages =
                        std::min(remaining_pages, manager.GetPageOffsetToEnd(cur_address));
                    any_new = manager.ProcessOptimizedAllocation(m_system.Kernel(), cur_address,
                                                                 cur_pages, fill_pattern);

                    // Advance.
                    cur_address += cur_pages * PageSize;
                    remaining_pages -= cur_pages;
                }
            }

            // If there are new pages, update tracking for the allocation.
            if (any_new) {
                // Update tracking for the allocation.
                KPhysicalAddress cur_address = block_address;
                size_t remaining_pages = block_pages;
                while (remaining_pages > 0) {
                    // Get the manager for the current address.
                    auto& manager = this->GetManager(cur_address);

                    // Lock the pool for the manager.
                    KScopedLightLock lk(m_pool_locks[static_cast<size_t>(manager.GetPool())]);

                    // Track some or all of the current pages.
                    const size_t cur_pages =
                        std::min(remaining_pages, manager.GetPageOffsetToEnd(cur_address));
                    manager.TrackOptimizedAllocation(m_system.Kernel(), cur_address, cur_pages);

                    // Advance.
                    cur_address += cur_pages * PageSize;
                    remaining_pages -= cur_pages;
                }
            }
        }
    } else {
        // Set all the allocated memory.
        for (const auto& block : *out) {
            m_system.DeviceMemory().buffer.ClearBackingRegion(block.GetAddress().GetValue() -
                                                                  Core::DramMemoryMap::Base,
                                                              block.GetSize(), fill_pattern);
        }
    }

    ReleaseFreeBacking();
    R_SUCCEED();
}

size_t KMemoryManager::Impl::Initialize(KPhysicalAddress address, size_t size,
                                        KVirtualAddress management, KVirtualAddress management_end,
                                        Pool p) {
    // Calculate management sizes.
    const size_t ref_count_size = (size / PageSize) * sizeof(u16);
    const size_t optimize_map_size = CalculateOptimizedProcessOverheadSize(size);
    const size_t manager_size = Common::AlignUp(optimize_map_size + ref_count_size, PageSize);
    const size_t page_heap_size = KPageHeap::CalculateManagementOverheadSize(size);
    const size_t total_management_size = manager_size + page_heap_size;
    ASSERT(manager_size <= total_management_size);
    ASSERT(management + total_management_size <= management_end);
    ASSERT(Common::IsAligned(total_management_size, PageSize));

    // Setup region.
    m_pool = p;
    m_management_region = management;
    m_page_reference_counts.resize(size / PageSize);
    ASSERT(Common::IsAligned(m_management_region.GetValue(), PageSize));

    // Initialize the manager's KPageHeap.
    m_heap.Initialize(address, size, management + manager_size, page_heap_size);

    return total_management_size;
}

void KMemoryManager::Impl::InitializeOptimizedMemory(KernelCore& kernel) {
    auto optimize_pa = KPageTable::GetHeapPhysicalAddress(kernel, m_management_region);
    const size_t optimize_size = CalculateOptimizedProcessOverheadSize(m_heap.GetSize());
#ifdef _WIN32
    // DramPoolManagement lies beyond the 64 MiB bootstrap commit window under lazy guest commit.
    kernel.System().DeviceMemory().buffer.CommitBackingRegion(
        optimize_pa.GetValue() - Core::DramMemoryMap::Base, optimize_size);
#endif
    auto* optimize_map = kernel.System().DeviceMemory().GetPointer<u64>(optimize_pa);

    std::memset(optimize_map, 0, optimize_size);
}

void KMemoryManager::Impl::TrackUnoptimizedAllocation(KernelCore& kernel, KPhysicalAddress block,
                                                      size_t num_pages) {
    auto optimize_pa = KPageTable::GetHeapPhysicalAddress(kernel, m_management_region);
    auto* optimize_map = kernel.System().DeviceMemory().GetPointer<u64>(optimize_pa);

    // Get the range we're tracking.
    size_t offset = this->GetPageOffset(block);
    const size_t last = offset + num_pages - 1;

    // Track.
    while (offset <= last) {
        // Mark the page as not being optimized-allocated.
        optimize_map[offset / Common::BitSize<u64>()] &=
            ~(u64(1) << (offset % Common::BitSize<u64>()));

        offset++;
    }
}

void KMemoryManager::Impl::TrackOptimizedAllocation(KernelCore& kernel, KPhysicalAddress block,
                                                    size_t num_pages) {
    auto optimize_pa = KPageTable::GetHeapPhysicalAddress(kernel, m_management_region);
    auto* optimize_map = kernel.System().DeviceMemory().GetPointer<u64>(optimize_pa);

    // Get the range we're tracking.
    size_t offset = this->GetPageOffset(block);
    const size_t last = offset + num_pages - 1;

    // Track.
    while (offset <= last) {
        // Mark the page as being optimized-allocated.
        optimize_map[offset / Common::BitSize<u64>()] |=
            (u64(1) << (offset % Common::BitSize<u64>()));

        offset++;
    }
}

bool KMemoryManager::Impl::ProcessOptimizedAllocation(KernelCore& kernel, KPhysicalAddress block,
                                                      size_t num_pages, u8 fill_pattern) {
    auto& device_memory = kernel.System().DeviceMemory();
    auto optimize_pa = KPageTable::GetHeapPhysicalAddress(kernel, m_management_region);
    auto* optimize_map = device_memory.GetPointer<u64>(optimize_pa);

    // We want to return whether any pages were newly allocated.
    bool any_new = false;

    // Get the range we're processing.
    size_t offset = this->GetPageOffset(block);
    const size_t last = offset + num_pages - 1;

    // Process.
    while (offset <= last) {
        // Check if the page has been optimized-allocated before.
        if ((optimize_map[offset / Common::BitSize<u64>()] &
             (u64(1) << (offset % Common::BitSize<u64>()))) == 0) {
            // If not, it's new.
            any_new = true;

            // Fill the page.
            auto* ptr = device_memory.GetPointer<u8>(m_heap.GetAddress());
            std::memset(ptr + offset * PageSize, fill_pattern, PageSize);
        }

        offset++;
    }

    // Return the number of pages we processed.
    return any_new;
}

size_t KMemoryManager::Impl::CalculateManagementOverheadSize(size_t region_size) {
    const size_t ref_count_size = (region_size / PageSize) * sizeof(u16);
    const size_t optimize_map_size =
        (Common::AlignUp((region_size / PageSize), Common::BitSize<u64>()) /
         Common::BitSize<u64>()) *
        sizeof(u64);
    const size_t manager_meta_size = Common::AlignUp(optimize_map_size + ref_count_size, PageSize);
    const size_t page_heap_size = KPageHeap::CalculateManagementOverheadSize(region_size);
    return manager_meta_size + page_heap_size;
}

} // namespace Kernel
