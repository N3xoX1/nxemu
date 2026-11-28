// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-FileCopyrightText: 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "core/core.h"
#include "core/device_memory.h"
#include "core/gpu_dirty_memory_manager.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "core/nce_write_streams.h"
#include "core/nce_written_ranges.h"
#include "nxemu-video/video_settings_identifiers.h"
#include "os_settings.h"
#include "yuzu_common/atomic_ops.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/hardware_properties.h"
#include "yuzu_common/heap_tracker.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/make_unique_for_overwrite.h"
#include "yuzu_common/page_table.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/swap.h"
#include "yuzu_common/yuzu_assert.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-module-spec/video.h>

extern IModuleSettings * g_settings;

namespace Core::Memory
{

static bool ProtectOnlyReadCachedPages()
{
#if !defined(__APPLE__)
    // Vulkan only downloads images on a CPU read, OpenGL also downloads buffers
    static const bool enabled = osSettings.use_reactive_flushing &&
                                g_settings->GetInt(NXVideoSetting::GraphicsAPI) == static_cast<int32_t>(RendererBackend::Vulkan);
    return enabled;
#else
    return false;
#endif
}

static bool UseWriteTrackMap()
{
#if defined(_M_X64) || defined(__x86_64__)
    return ProtectOnlyReadCachedPages();
#else
    return false;
#endif
}

namespace
{

bool AddressSpaceContains(const Common::PageTable & table, const Common::ProcessAddress addr,
                          const std::size_t size)
{
    const Common::ProcessAddress max_addr = 1ULL << table.GetAddressSpaceBits();
    return addr + size >= addr && addr + size <= max_addr;
}

} // namespace

// Implementation class used to keep the specifics of the memory subsystem hidden
namespace
{
// A fresh identifier for every Memory::Reset(), including when an Impl address is reused.
// Only touched when constructing the memory subsystem, never during guest reads.
std::atomic<u64> next_rasterizer_readback_generation{1};
} // namespace

// from outside classes. This also allows modification to the internals of the memory
// subsystem without needing to rebuild all files that make use of the memory interface.
struct Memory::Impl
{
    explicit Impl(Core::System & system_) :
        system{system_},
        rasterizer_readback_generation{
            next_rasterizer_readback_generation.fetch_add(1, std::memory_order_relaxed)}
    {
    }

    void SetCurrentPageTable(Kernel::KProcess & process)
    {
        current_page_table = &process.GetKPageTable().GetImpl();

        const bool fastmem = !g_settings->GetBool(NXCpuSetting::CpuDebugMode) ||
                             g_settings->GetBool(NXCpuSetting::CpuoptFastmem);
        if (std::addressof(process) == system.ApplicationProcess() && fastmem)
        {
            current_page_table->fastmem_arena = system.DeviceMemory().buffer.VirtualBasePointer();
            if (UseWriteTrackMap())
            {
                current_page_table->EnableWriteTracking();
            }
        }
        else
        {
            current_page_table->fastmem_arena = nullptr;
        }

#ifdef __linux__
        heap_tracker.emplace(system.DeviceMemory().buffer);
        buffer = std::addressof(*heap_tracker);
#else
        buffer = std::addressof(system.DeviceMemory().buffer);
#endif
    }

    void MapMemoryRegion(Common::PageTable & page_table, Common::ProcessAddress base, u64 size,
                         Common::PhysicalAddress target, Common::MemoryPermission perms,
                         bool separate_heap)
    {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", base.GetValue());
        ASSERT_MSG(target >= DramMemoryMap::Base, "Out of bounds target: {:016X}",
                   target.GetValue());
        MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, target,
                 Common::PageType::Memory);

        if (current_page_table->fastmem_arena)
        {
            buffer->Map(base.GetValue(), target.GetValue() - DramMemoryMap::Base, size, perms,
                        separate_heap);
        }
    }

    void UnmapRegion(Common::PageTable & page_table, Common::ProcessAddress base, u64 size,
                     bool separate_heap)
    {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", base.GetValue());
#if defined(__APPLE__)
        std::vector<std::pair<const u8 *, u64>> unmap_written;
        {
            std::unique_lock lk{nce_write_mutex};
            if (size != 0)
            {
                const u64 begin = base.GetValue();
                const u64 end = begin + size;
                WaitForActiveGpuReadbacksForVAddrLocked(lk, begin, end);
                for (auto it = nce_written.lower_bound(begin);
                     it != nce_written.end() && it->first < end;)
                {
                    if (const u8 * const ptr = GetPointerFromRasterizerCachedMemory(it->first))
                    {
                        ForEachNceWrittenRange(
                            ptr, it->second.copy->data(), YUZU_PAGESIZE,
                            [&](u64 offset, u64 range_size) {
                                unmap_written.emplace_back(ptr + offset, range_size);
                            });
                    }
                    ErasePhysEntry(nce_written_by_phys, it->second.phys_page, it->first);
                    ReleasePageCopy(std::move(it->second.copy));
                    it = nce_written.erase(it);
                }
                if (nce_written.empty())
                {
                    has_nce_written.store(false, std::memory_order_release);
                }
                for (auto it = nce_flushing.lower_bound(begin);
                     it != nce_flushing.end() && it->first < end;)
                {
                    ErasePhysEntry(nce_flushing_by_phys, it->second.phys_page, it->first);
                    ReleasePageCopy(std::move(it->second.copy));
                    it = nce_flushing.erase(it);
                }
                if (nce_flushing.empty())
                {
                    has_nce_flushing.store(false, std::memory_order_release);
                }
                for (auto it = nce_open_read_pages.lower_bound(begin);
                     it != nce_open_read_pages.end() && *it < end;)
                {
                    it = nce_open_read_pages.erase(it);
                }
                if (nce_open_read_pages.empty())
                {
                    has_nce_open_read_pages.store(false, std::memory_order_release);
                }
                for (auto it = nce_read_cached.lower_bound(begin);
                     it != nce_read_cached.end() && it->first < end;)
                {
                    it = nce_read_cached.erase(it);
                }
                for (auto it = nce_ever_direct_read_pages.lower_bound(begin);
                     it != nce_ever_direct_read_pages.end() && *it < end;)
                {
                    it = nce_ever_direct_read_pages.erase(it);
                }
            }
            MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, 0,
                     Common::PageType::Unmapped);

            if (current_page_table->fastmem_arena)
            {
                buffer->Unmap(base.GetValue(), size, separate_heap);
            }
        }
        if (!unmap_written.empty())
        {
            constexpr size_t sys_core = Hardware::NUM_CPU_CORES - 1;
            const size_t core = std::min(system.GetCurrentHostThreadID(), sys_core);
            std::unique_lock sys_lock{sys_core_guard, std::defer_lock};
            if (core == sys_core) [[unlikely]]
            {
                sys_lock.lock();
            }
            RasterizerWriteCollectContext context{this, core};
            for (const auto & [phys_ptr, range_size] : unmap_written)
            {
                system.GetVideo().HandleRasterizerWrite(
                    phys_ptr, range_size, &rasterizer_write_areas[core].last_address,
                    HandleRasterizerWriteCollectExact, &context);
            }
        }
#else
        MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, 0,
                 Common::PageType::Unmapped);

        if (current_page_table->fastmem_arena)
        {
            buffer->Unmap(base.GetValue(), size, separate_heap);
        }
#endif
    }

    void ProtectRegion(Common::PageTable & page_table, VAddr vaddr, u64 size,
                       Common::MemoryPermission perms)
    {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((vaddr & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", vaddr);

        if (!current_page_table->fastmem_arena)
        {
            return;
        }

#if defined(__APPLE__)
        // The GPU cache keeps its own access mask there, so the guest permission applies to
        // every page.
        buffer->Protect(vaddr, size, perms);
        return;
#endif

        u64 protect_bytes{};
        u64 protect_begin{};
        for (u64 addr = vaddr; addr < vaddr + size; addr += YUZU_PAGESIZE)
        {
            const Common::PageType page_type{
                current_page_table->pointers[addr >> YUZU_PAGEBITS].Type()};
            switch (page_type)
            {
            case Common::PageType::RasterizerCachedMemory:
                if (protect_bytes > 0)
                {
                    buffer->Protect(protect_begin, protect_bytes, perms);
                    protect_bytes = 0;
                }
                break;
            default:
                if (protect_bytes == 0)
                {
                    protect_begin = addr;
                }
                protect_bytes += YUZU_PAGESIZE;
            }
        }

        if (protect_bytes > 0)
        {
            buffer->Protect(protect_begin, protect_bytes, perms);
        }
    }

    [[nodiscard]] u8 * GetPointerFromRasterizerCachedMemory(u64 vaddr) const
    {
        const Common::PhysicalAddress paddr{
            current_page_table->backing_addr[vaddr >> YUZU_PAGEBITS]};

        if (!paddr)
        {
            return {};
        }

        return system.DeviceMemory().GetPointer<u8>(paddr + vaddr);
    }

    [[nodiscard]] u8 * GetPointerFromDebugMemory(u64 vaddr) const
    {
        const Common::PhysicalAddress paddr{
            current_page_table->backing_addr[vaddr >> YUZU_PAGEBITS]};

        if (paddr == 0)
        {
            return {};
        }

        return system.DeviceMemory().GetPointer<u8>(paddr + vaddr);
    }

    u8 Read8(const Common::ProcessAddress addr)
    {
        return Read<u8>(addr);
    }

    u16 Read16(const Common::ProcessAddress addr)
    {
        if ((addr & 1) == 0)
        {
            return Read<u16_le>(addr);
        }
        else
        {
            const u32 a{Read<u8>(addr)};
            const u32 b{Read<u8>(addr + sizeof(u8))};
            return static_cast<u16>((b << 8) | a);
        }
    }

    u32 Read32(const Common::ProcessAddress addr)
    {
        if ((addr & 3) == 0)
        {
            return Read<u32_le>(addr);
        }
        else
        {
            const u32 a{Read16(addr)};
            const u32 b{Read16(addr + sizeof(u16))};
            return (b << 16) | a;
        }
    }

    u64 Read64(const Common::ProcessAddress addr)
    {
        if ((addr & 7) == 0)
        {
            return Read<u64_le>(addr);
        }
        else
        {
            const u32 a{Read32(addr)};
            const u32 b{Read32(addr + sizeof(u32))};
            return (static_cast<u64>(b) << 32) | a;
        }
    }

    void Write8(const Common::ProcessAddress addr, const u8 data)
    {
        Write<u8>(addr, data);
    }

    void Write16(const Common::ProcessAddress addr, const u16 data)
    {
        if ((addr & 1) == 0)
        {
            Write<u16_le>(addr, data);
        }
        else
        {
            Write<u8>(addr, static_cast<u8>(data));
            Write<u8>(addr + sizeof(u8), static_cast<u8>(data >> 8));
        }
    }

    void Write32(const Common::ProcessAddress addr, const u32 data)
    {
        if ((addr & 3) == 0)
        {
            Write<u32_le>(addr, data);
        }
        else
        {
            Write16(addr, static_cast<u16>(data));
            Write16(addr + sizeof(u16), static_cast<u16>(data >> 16));
        }
    }

    void Write64(const Common::ProcessAddress addr, const u64 data)
    {
        if ((addr & 7) == 0)
        {
            Write<u64_le>(addr, data);
        }
        else
        {
            Write32(addr, static_cast<u32>(data));
            Write32(addr + sizeof(u32), static_cast<u32>(data >> 32));
        }
    }

    bool WriteExclusive8(const Common::ProcessAddress addr, const u8 data, const u8 expected)
    {
        return WriteExclusive<u8>(addr, data, expected);
    }

    bool WriteExclusive16(const Common::ProcessAddress addr, const u16 data, const u16 expected)
    {
        return WriteExclusive<u16_le>(addr, data, expected);
    }

    bool WriteExclusive32(const Common::ProcessAddress addr, const u32 data, const u32 expected)
    {
        return WriteExclusive<u32_le>(addr, data, expected);
    }

    bool WriteExclusive64(const Common::ProcessAddress addr, const u64 data, const u64 expected)
    {
        return WriteExclusive<u64_le>(addr, data, expected);
    }

    std::string ReadCString(Common::ProcessAddress vaddr, std::size_t max_length)
    {
        std::string string;
        string.reserve(max_length);
        for (std::size_t i = 0; i < max_length; ++i)
        {
            const char c = Read<s8>(vaddr);
            if (c == '\0')
            {
                break;
            }
            string.push_back(c);
            ++vaddr;
        }
        string.shrink_to_fit();
        return string;
    }

    bool WalkBlock(const Common::ProcessAddress addr, const std::size_t size, auto on_unmapped, auto on_memory, auto on_rasterizer, auto increment)
    {
        const auto & page_table = *current_page_table;
        std::size_t remaining_size = size;
        std::size_t page_index = addr >> YUZU_PAGEBITS;
        std::size_t page_offset = addr & YUZU_PAGEMASK;
        bool user_accessible = true;

        if (!AddressSpaceContains(page_table, addr, size)) [[unlikely]]
        {
            on_unmapped(size, addr);
            return false;
        }

        while (remaining_size)
        {
            const std::size_t copy_amount =
                std::min(static_cast<std::size_t>(YUZU_PAGESIZE) - page_offset, remaining_size);
            const auto current_vaddr =
                static_cast<u64>((page_index << YUZU_PAGEBITS) + page_offset);

            const auto [pointer, type] = page_table.pointers[page_index].PointerType();
            switch (type)
            {
            case Common::PageType::Unmapped:
            {
                user_accessible = false;
                on_unmapped(copy_amount, current_vaddr);
                break;
            }
            case Common::PageType::Memory:
            {
                u8 * mem_ptr =
                    reinterpret_cast<u8 *>(pointer + page_offset + (page_index << YUZU_PAGEBITS));
                on_memory(copy_amount, mem_ptr);
                break;
            }
            case Common::PageType::DebugMemory:
            {
                u8 * const mem_ptr{GetPointerFromDebugMemory(current_vaddr)};
                on_memory(copy_amount, mem_ptr);
                break;
            }
            case Common::PageType::RasterizerCachedMemory:
            {
                u8 * const host_ptr{GetPointerFromRasterizerCachedMemory(current_vaddr)};
                on_rasterizer(current_vaddr, copy_amount, host_ptr);
                break;
            }
            default:
                UNREACHABLE();
            }

            page_index++;
            page_offset = 0;
            increment(copy_amount);
            remaining_size -= copy_amount;
        }

        return user_accessible;
    }

    template <bool UNSAFE>
    bool ReadBlockImpl(const Common::ProcessAddress src_addr, void * dest_buffer,
                       const std::size_t size)
    {
        return WalkBlock(
            src_addr, size,
            [src_addr, size, &dest_buffer](const std::size_t copy_amount,
                                           const Common::ProcessAddress current_vaddr) {
                LOG_ERROR(HW_Memory,
                          "Unmapped ReadBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr.GetValue(), src_addr.GetValue(), size);
                std::memset(dest_buffer, 0, copy_amount);
            },
            [&](const std::size_t copy_amount, const u8 * const src_ptr) {
                std::memcpy(dest_buffer, src_ptr, copy_amount);
            },
            [&](const Common::ProcessAddress current_vaddr, const std::size_t copy_amount,
                const u8 * const host_ptr) {
                if constexpr (!UNSAFE)
                {
                    HandleRasterizerDownload(current_vaddr.GetValue(), copy_amount);
                }
                std::memcpy(dest_buffer, host_ptr, copy_amount);
            },
            [&](const std::size_t copy_amount) {
                dest_buffer = static_cast<u8 *>(dest_buffer) + copy_amount;
            });
    }

    bool ReadBlock(const Common::ProcessAddress src_addr, void * dest_buffer, const std::size_t size)
    {
        return ReadBlockImpl<false>(src_addr, dest_buffer, size);
    }

    bool ReadBlockUnsafe(const Common::ProcessAddress src_addr, void * dest_buffer, const std::size_t size)
    {
        return ReadBlockImpl<true>(src_addr, dest_buffer, size);
    }

    u8 * GetSpan(const VAddr src_addr, const std::size_t size) const
    {
        // Check the half-open range, including the zero-length case at the address-space limit.
        if (!AddressSpaceContains(*current_page_table, src_addr, size != 0 ? size : 1))
        {
            return nullptr;
        }
        u8 * const first = GetPointerSilent(src_addr);
        if (!first)
        {
            return nullptr;
        }
        const u64 first_page = src_addr >> YUZU_PAGEBITS;
        const u64 last_page = (src_addr + (size != 0 ? size - 1 : 0)) >> YUZU_PAGEBITS;
        const uintptr_t host_offset = reinterpret_cast<uintptr_t>(first) - src_addr;
        for (u64 page = first_page + 1; page <= last_page; ++page)
        {
            const uintptr_t offset = current_page_table->pointers[page].Pointer();
            if (offset != 0)
            {
                if (offset != host_offset)
                {
                    return nullptr;
                }
            }
            else
            {
                // Cached/debug pages may have no direct pointer in the page table.
                const u64 addr = page << YUZU_PAGEBITS;
                const u8 * const ptr = GetPointerSilent(addr);
                if (!ptr || reinterpret_cast<uintptr_t>(ptr) - addr != host_offset)
                {
                    return nullptr;
                }
            }
        }
        return first;
    }

    template <bool UNSAFE>
    bool WriteBlockImpl(const Common::ProcessAddress dest_addr, const void * src_buffer,
                        const std::size_t size)
    {
        return WalkBlock(
            dest_addr, size,
            [dest_addr, size](const std::size_t copy_amount,
                              const Common::ProcessAddress current_vaddr) {
                LOG_ERROR(HW_Memory,
                          "Unmapped WriteBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr.GetValue(), dest_addr.GetValue(), size);
            },
            [&](const std::size_t copy_amount, u8 * const dest_ptr) {
                std::memcpy(dest_ptr, src_buffer, copy_amount);
            },
            [&](const Common::ProcessAddress current_vaddr, const std::size_t copy_amount,
                u8 * const host_ptr) {
                if constexpr (!UNSAFE)
                {
                    HandleRasterizerWrite(current_vaddr.GetValue(), copy_amount);
                }
                std::memcpy(host_ptr, src_buffer, copy_amount);
            },
            [&](const std::size_t copy_amount) {
                src_buffer = static_cast<const u8 *>(src_buffer) + copy_amount;
            });
    }

    bool WriteBlock(const Common::ProcessAddress dest_addr, const void * src_buffer,
                    const std::size_t size)
    {
        return WriteBlockImpl<false>(dest_addr, src_buffer, size);
    }

    bool WriteBlockUnsafe(const Common::ProcessAddress dest_addr, const void * src_buffer,
                          const std::size_t size)
    {
        return WriteBlockImpl<true>(dest_addr, src_buffer, size);
    }

    bool ZeroBlock(const Common::ProcessAddress dest_addr, const std::size_t size)
    {
        return WalkBlock(
            dest_addr, size,
            [dest_addr, size](const std::size_t copy_amount,
                              const Common::ProcessAddress current_vaddr) {
                LOG_ERROR(HW_Memory,
                          "Unmapped ZeroBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr.GetValue(), dest_addr.GetValue(), size);
            },
            [](const std::size_t copy_amount, u8 * const dest_ptr) {
                std::memset(dest_ptr, 0, copy_amount);
            },
            [&](const Common::ProcessAddress current_vaddr, const std::size_t copy_amount,
                u8 * const host_ptr) {
                HandleRasterizerWrite(current_vaddr.GetValue(), copy_amount);
                std::memset(host_ptr, 0, copy_amount);
            },
            [](const std::size_t copy_amount) {});
    }

    bool CopyBlock(Common::ProcessAddress dest_addr, Common::ProcessAddress src_addr,
                   const std::size_t size)
    {
        return WalkBlock(
            dest_addr, size,
            [&](const std::size_t copy_amount, const Common::ProcessAddress current_vaddr) {
                LOG_ERROR(HW_Memory,
                          "Unmapped CopyBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr.GetValue(), src_addr.GetValue(), size);
                ZeroBlock(dest_addr, copy_amount);
            },
            [&](const std::size_t copy_amount, const u8 * const src_ptr) {
                WriteBlockImpl<false>(dest_addr, src_ptr, copy_amount);
            },
            [&](const Common::ProcessAddress current_vaddr, const std::size_t copy_amount,
                u8 * const host_ptr) {
                HandleRasterizerDownload(current_vaddr.GetValue(), copy_amount);
                WriteBlockImpl<false>(dest_addr, host_ptr, copy_amount);
            },
            [&](const std::size_t copy_amount) {
                dest_addr += copy_amount;
                src_addr += copy_amount;
            });
    }

    template <typename Callback>
    Result PerformCacheOperation(Common::ProcessAddress dest_addr, std::size_t size,
                                 Callback && cb)
    {
        class InvalidMemoryException : public std::exception
        {
        };

        try
        {
            WalkBlock(
                dest_addr, size,
                [&](const std::size_t block_size, const Common::ProcessAddress current_vaddr) {
                    LOG_ERROR(HW_Memory, "Unmapped cache maintenance @ {:#018X}",
                              current_vaddr.GetValue());
                    throw InvalidMemoryException();
                },
                [&](const std::size_t block_size, u8 * const host_ptr) {},
                [&](const Common::ProcessAddress current_vaddr, const std::size_t block_size,
                    u8 * const host_ptr) { cb(current_vaddr, block_size); },
                [](const std::size_t block_size) {});
        }
        catch (InvalidMemoryException &)
        {
            return Kernel::ResultInvalidCurrentMemory;
        }

        return ResultSuccess;
    }

    Result InvalidateDataCache(Common::ProcessAddress dest_addr, std::size_t size)
    {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr,
                                 const std::size_t block_size) {
            // dc ivac: Invalidate to point of coherency
            // GPU flush -> CPU invalidate
            HandleRasterizerDownload(current_vaddr.GetValue(), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    Result StoreDataCache(Common::ProcessAddress dest_addr, std::size_t size)
    {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr,
                                 const std::size_t block_size) {
            // dc cvac: Store to point of coherency
            // CPU flush -> GPU invalidate
            HandleRasterizerWrite(current_vaddr.GetValue(), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    Result FlushDataCache(Common::ProcessAddress dest_addr, std::size_t size)
    {
        auto on_rasterizer = [&](const Common::ProcessAddress current_vaddr,
                                 const std::size_t block_size) {
            // dc civac: Store to point of coherency, and invalidate from cache
            // CPU flush -> GPU invalidate
            HandleRasterizerWrite(current_vaddr.GetValue(), block_size);
        };
        return PerformCacheOperation(dest_addr, size, on_rasterizer);
    }

    void MarkRegionDebug(u64 vaddr, u64 size, bool debug)
    {
        if (vaddr == 0 || !AddressSpaceContains(*current_page_table, vaddr, size))
        {
            return;
        }

        if (current_page_table->fastmem_arena)
        {
            const auto perm{debug ? Common::MemoryPermission{}
                                  : Common::MemoryPermission::ReadWrite};
#if defined(__APPLE__)
            buffer->ProtectAccess(vaddr, size, perm);
#else
            buffer->Protect(vaddr, size, perm);
#endif
        }

        // Iterate over a contiguous CPU address space, marking/unmarking the region.
        // The region is at a granularity of CPU pages.

        const u64 num_pages = ((vaddr + size - 1) >> YUZU_PAGEBITS) - (vaddr >> YUZU_PAGEBITS) + 1;
        for (u64 i = 0; i < num_pages; ++i, vaddr += YUZU_PAGESIZE)
        {
            const Common::PageType page_type{
                current_page_table->pointers[vaddr >> YUZU_PAGEBITS].Type()};
            if (debug)
            {
                // Switch page type to debug if now debug
                switch (page_type)
                {
                case Common::PageType::Unmapped:
                    ASSERT_MSG(false, "Attempted to mark unmapped pages as debug");
                    break;
                case Common::PageType::RasterizerCachedMemory:
                case Common::PageType::DebugMemory:
                    // Page is already marked.
                    break;
                case Common::PageType::Memory:
                    current_page_table->pointers[vaddr >> YUZU_PAGEBITS].Store(
                        0, Common::PageType::DebugMemory);
                    break;
                default:
                    UNREACHABLE();
                }
            }
            else
            {
                // Switch page type to non-debug if now non-debug
                switch (page_type)
                {
                case Common::PageType::Unmapped:
                    ASSERT_MSG(false, "Attempted to mark unmapped pages as non-debug");
                    break;
                case Common::PageType::RasterizerCachedMemory:
                case Common::PageType::Memory:
                    // Don't mess with already non-debug or rasterizer memory.
                    break;
                case Common::PageType::DebugMemory:
                {
                    u8 * const pointer{GetPointerFromDebugMemory(vaddr & ~YUZU_PAGEMASK)};
                    current_page_table->pointers[vaddr >> YUZU_PAGEBITS].Store(
                        reinterpret_cast<uintptr_t>(pointer) - (vaddr & ~YUZU_PAGEMASK),
                        Common::PageType::Memory);
                    break;
                }
                default:
                    UNREACHABLE();
                }
            }
        }
    }

    void RasterizerMarkRegionCached(u64 vaddr, u64 size, bool cached)
    {
        if (vaddr == 0 || size == 0 || !AddressSpaceContains(*current_page_table, vaddr, size))
        {
            return;
        }
#if defined(__APPLE__)
        std::unique_lock nce_lock{nce_write_mutex};
        if (!cached)
        {
            WaitForActiveGpuReadbacksForVAddrLocked(
                nce_lock, Common::AlignDown(vaddr, YUZU_PAGESIZE),
                Common::AlignUp(vaddr + size, YUZU_PAGESIZE));
        }
#else
        std::scoped_lock read_cached_lock{read_cached_mutex};
#endif

        // When caching a region, mark page table entries as RasterizerCachedMemory
        // BEFORE tightening NCE page protections so concurrent software Memory::Write*
        // / InterpreterVisitor writes never see PageType::Memory and skip GPU invalidation.
        const u64 orig_vaddr = vaddr;
        const u64 num_pages = ((vaddr + size - 1) >> YUZU_PAGEBITS) - (vaddr >> YUZU_PAGEBITS) + 1;
        if (cached)
        {
            u64 page_vaddr = orig_vaddr;
            for (u64 i = 0; i < num_pages; ++i, page_vaddr += YUZU_PAGESIZE)
            {
                const Common::PageType page_type{
                    current_page_table->pointers[page_vaddr >> YUZU_PAGEBITS].Type()};
                switch (page_type)
                {
                case Common::PageType::Unmapped:
                case Common::PageType::RasterizerCachedMemory:
                    break;
                case Common::PageType::DebugMemory:
                case Common::PageType::Memory:
                    current_page_table->pointers[page_vaddr >> YUZU_PAGEBITS].Store(
                        0, Common::PageType::RasterizerCachedMemory);
                    break;
                default:
                    UNREACHABLE();
                }
            }
        }

#if !defined(__APPLE__)
        if (WriteTracking())
        {
            // CPU writes are marked in the write map. Only pages that need a download or hold
            // GPU-written buffer data are protected.
            ProtectTrackedPages(vaddr, size, cached);
        }
        else if (current_page_table->fastmem_arena && cached && ProtectOnlyReadCachedPages())
        {
            ProtectCachedRegion(vaddr, size);
        }
        else if (current_page_table->fastmem_arena)
#else
        if (current_page_table->fastmem_arena)
#endif
        {
            Common::MemoryPermission perm{};
            bool deny_reads = cached && osSettings.use_reactive_flushing;
#if defined(__APPLE__)
            // Buffer caches only need write tracking. Texture readback protection is applied
            // separately below, including when a texture overlaps an already cached buffer.
            deny_reads = deny_reads && !buffer->UsesNce();
#endif
            if (!deny_reads)
            {
                perm |= Common::MemoryPermission::Read;
            }
            if (!cached)
            {
                perm |= Common::MemoryPermission::Write;
            }
#if defined(__APPLE__)
            if (buffer->UsesNce())
            {
                if (cached)
                {
                    ProtectNceReadCached(vaddr, size);
                }
                else
                {
                    // Preserve read protection on pages still tracked in nce_read_cached
                    // (e.g. pending occlusion query results) and keep DebugMemory PROT_NONE.
                    const u64 range_begin = Common::AlignDown(vaddr, YUZU_PAGESIZE);
                    const u64 range_end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
                    const bool reactive = osSettings.use_reactive_flushing;
                    u64 run_begin{}, run_end{};
                    Common::MemoryPermission run_perm{};
                    const auto flush_run = [&] {
                        if (run_begin != run_end)
                        {
                            buffer->ProtectAccess(run_begin, run_end - run_begin, run_perm);
                        }
                        run_begin = run_end = 0;
                    };
                    auto it = nce_read_cached.lower_bound(range_begin);
                    for (u64 page = range_begin; page < range_end; page += YUZU_PAGESIZE)
                    {
                        while (it != nce_read_cached.end() && it->first < page)
                        {
                            ++it;
                        }
                        const bool read_cached = it != nce_read_cached.end() && it->first == page;
                        const auto page_type =
                            current_page_table->pointers[page >> YUZU_PAGEBITS].Type();
                        Common::MemoryPermission page_perm = perm;
                        if (page_type == Common::PageType::DebugMemory ||
                            (read_cached && reactive))
                        {
                            page_perm = Common::MemoryPermission{};
                        }
                        if (run_begin != run_end && (run_end != page || run_perm != page_perm))
                        {
                            flush_run();
                        }
                        if (run_begin == run_end)
                        {
                            run_begin = page;
                            run_perm = page_perm;
                        }
                        run_end = page + YUZU_PAGESIZE;
                    }
                    flush_run();
                }
            }
            else
            {
                buffer->ProtectAccess(vaddr, size, perm);
            }
#else
            buffer->Protect(vaddr, size, perm);
#endif
        }

        // Iterate over a contiguous CPU address space, which corresponds to the specified GPU
        // address space, marking the region as un/cached. The region is marked un/cached at a
        // granularity of CPU pages, hence why we iterate on a CPU page basis (note: GPU page size
        // is different). This assumes the specified GPU address region is contiguous as well.

        if (cached)
        {
#if !defined(__APPLE__)
            if (WriteTracking())
            {
                // After the pages are cached, so that a harvest keeps the marks of later writes. A
                // write in between loses its mark, but the caller uploads the region after this.
                ClearWriteMarks(orig_vaddr, size);
            }
#endif
            return;
        }
        for (u64 i = 0; i < num_pages; ++i, vaddr += YUZU_PAGESIZE)
        {
            const Common::PageType page_type{
                current_page_table->pointers[vaddr >> YUZU_PAGEBITS].Type()};
            // Switch page type to uncached if now uncached
            switch (page_type)
            {
            case Common::PageType::Unmapped: // NOLINT(bugprone-branch-clone)
                // It is not necessary for a process to have this region mapped into its address
                // space, for example, a system module need not have a VRAM mapping.
                break;
            case Common::PageType::DebugMemory:
            case Common::PageType::Memory:
                // There can be more than one GPU region mapped per CPU region, so it's common
                // that this area is already unmarked as cached.
                break;
            case Common::PageType::RasterizerCachedMemory:
            {
#if defined(__APPLE__)
                if (auto wit = nce_written.find(vaddr & ~YUZU_PAGEMASK);
                    wit != nce_written.end())
                {
                    ErasePhysEntry(nce_written_by_phys, wit->second.phys_page, wit->first);
                    ReleasePageCopy(std::move(wit->second.copy));
                    nce_written.erase(wit);
                    if (nce_written.empty())
                    {
                        has_nce_written.store(false, std::memory_order_release);
                    }
                }
#endif
                    u8 * const pointer{GetPointerFromRasterizerCachedMemory(vaddr & ~YUZU_PAGEMASK)};
                    if (pointer == nullptr)
                    {
                        // It's possible that this function has been called while updating the
                        // pagetable after unmapping a VMA. In that case the underlying VMA will no
                        // longer exist, and we should just leave the pagetable entry blank.
                        current_page_table->pointers[vaddr >> YUZU_PAGEBITS].Store(
                            0, Common::PageType::Unmapped);
                    }
                    else
                    {
                        current_page_table->pointers[vaddr >> YUZU_PAGEBITS].Store(
                            reinterpret_cast<uintptr_t>(pointer) - (vaddr & ~YUZU_PAGEMASK),
                            Common::PageType::Memory);
                    }
                    break;
                }
            default:
                UNREACHABLE();
            }
        }
    }

    /**
     * Maps a region of pages as a specific type.
     *
     * @param page_table The page table to use to perform the mapping.
     * @param base       The base address to begin mapping at.
     * @param size       The total size of the range in bytes.
     * @param target     The target address to begin mapping from.
     * @param type       The page type to map the memory as.
     */
    void MapPages(Common::PageTable & page_table, Common::ProcessAddress base_address, u64 size,
                  Common::PhysicalAddress target, Common::PageType type)
    {
        auto base = base_address.GetValue();

        LOG_DEBUG(HW_Memory, "Mapping {:016X} onto {:016X}-{:016X}", target.GetValue(),
                  base * YUZU_PAGESIZE, (base + size) * YUZU_PAGESIZE);

        const auto end = base + size;
        ASSERT_MSG(end <= page_table.pointers.size(), "out of range mapping at {:016X}",
                   base + page_table.pointers.size());

        if (!target)
        {
            ASSERT_MSG(type != Common::PageType::Memory,
                       "Mapping memory page without a pointer @ {:016x}", base * YUZU_PAGESIZE);

            while (base != end)
            {
                page_table.pointers[base].Store(0, type);
                page_table.backing_addr[base] = 0;
                page_table.blocks[base] = 0;
                base += 1;
            }
        }
        else
        {
            auto orig_base = base;
            while (base != end)
            {
                auto host_ptr =
                    reinterpret_cast<uintptr_t>(system.DeviceMemory().GetPointer<u8>(target)) -
                    (base << YUZU_PAGEBITS);
                auto backing = target.GetValue() - (base << YUZU_PAGEBITS);
                page_table.pointers[base].Store(host_ptr, type);
                page_table.backing_addr[base] = backing;
                page_table.blocks[base] = orig_base << YUZU_PAGEBITS;

                ASSERT_MSG(page_table.pointers[base].Pointer(),
                           "memory mapping base yield a nullptr within the table");

                base += 1;
                target += YUZU_PAGESIZE;
            }
        }
    }

    [[nodiscard]] u8 * GetPointerImpl(u64 vaddr, auto on_unmapped, auto on_rasterizer) const
    {
        // AARCH64 masks the upper 16 bit of all memory accesses
        vaddr = vaddr & 0xffffffffffffULL;

        if (!AddressSpaceContains(*current_page_table, vaddr, 1)) [[unlikely]]
        {
            on_unmapped();
            return nullptr;
        }

        // Avoid adding any extra logic to this fast-path block
        const uintptr_t raw_pointer = current_page_table->pointers[vaddr >> YUZU_PAGEBITS].Raw();
        if (const uintptr_t pointer = Common::PageTable::PageInfo::ExtractPointer(raw_pointer))
        {
            return reinterpret_cast<u8 *>(pointer + vaddr);
        }
        switch (Common::PageTable::PageInfo::ExtractType(raw_pointer))
        {
        case Common::PageType::Unmapped:
            on_unmapped();
            return nullptr;
        case Common::PageType::Memory:
            ASSERT_MSG(false, "Mapped memory page without a pointer @ 0x{:016X}", vaddr);
            return nullptr;
        case Common::PageType::DebugMemory:
            return GetPointerFromDebugMemory(vaddr);
        case Common::PageType::RasterizerCachedMemory:
        {
            u8 * const host_ptr{GetPointerFromRasterizerCachedMemory(vaddr)};
            on_rasterizer();
            return host_ptr;
        }
        default:
            UNREACHABLE();
        }
        return nullptr;
    }

    [[nodiscard]] u8 * GetPointer(const Common::ProcessAddress vaddr) const
    {
        return GetPointerImpl(
            vaddr.GetValue(),
            [vaddr]() {
                LOG_ERROR(HW_Memory, "Unmapped GetPointer @ 0x{:016X}", vaddr.GetValue());
            },
            []() {});
    }

    [[nodiscard]] u8 * GetPointerSilent(const Common::ProcessAddress vaddr) const
    {
        return GetPointerImpl(
            vaddr.GetValue(), []() {}, []() {});
    }

    /**
     * Reads a particular data type out of memory at the given virtual address.
     *
     * @param vaddr The virtual address to read the data type from.
     *
     * @tparam T The data type to read out of memory. This type *must* be
     *           trivially copyable, otherwise the behavior of this function
     *           is undefined.
     *
     * @returns The instance of T read from the specified virtual address.
     */
    template <typename T>
    T Read(Common::ProcessAddress vaddr)
    {
        T result = 0;
        const u8 * const ptr = GetPointerImpl(
            vaddr.GetValue(),
            [vaddr]() {
                LOG_ERROR(HW_Memory, "Unmapped Read{} @ 0x{:016X}", sizeof(T) * 8,
                          vaddr.GetValue());
            },
            [&]() { HandleRasterizerDownload(vaddr.GetValue(), sizeof(T)); });
        if (ptr)
        {
            std::memcpy(&result, ptr, sizeof(T));
        }
        return result;
    }

    /**
     * Writes a particular data type to memory at the given virtual address.
     *
     * @param vaddr The virtual address to write the data type to.
     *
     * @tparam T The data type to write to memory. This type *must* be
     *           trivially copyable, otherwise the behavior of this function
     *           is undefined.
     */
    template <typename T>
    void Write(Common::ProcessAddress vaddr, const T data)
    {
        u8 * const ptr = GetPointerImpl(
            vaddr.GetValue(),
            [vaddr, data]() {
                LOG_ERROR(HW_Memory, "Unmapped Write{} @ 0x{:016X} = 0x{:016X}", sizeof(T) * 8,
                          vaddr.GetValue(), static_cast<u64>(data));
            },
            [&]() { HandleRasterizerWrite(vaddr.GetValue(), sizeof(T)); });
        if (ptr)
        {
            std::memcpy(ptr, &data, sizeof(T));
        }
    }

    template <typename T>
    bool WriteExclusive(Common::ProcessAddress vaddr, const T data, const T expected)
    {
        u8 * const ptr = GetPointerImpl(
            vaddr.GetValue(),
            [vaddr, data]() {
                LOG_ERROR(HW_Memory, "Unmapped WriteExclusive{} @ 0x{:016X} = 0x{:016X}",
                          sizeof(T) * 8, vaddr.GetValue(), static_cast<u64>(data));
            },
            [&]() { HandleRasterizerWrite(vaddr.GetValue(), sizeof(T)); });
        if (ptr)
        {
            return Common::AtomicCompareAndSwap(reinterpret_cast<T *>(ptr), data, expected);
        }
        return true;
    }

    bool WriteExclusive128(Common::ProcessAddress vaddr, const u128 data, const u128 expected)
    {
        u8 * const ptr = GetPointerImpl(
            vaddr.GetValue(),
            [vaddr, data]() {
                LOG_ERROR(HW_Memory, "Unmapped WriteExclusive128 @ 0x{:016X} = 0x{:016X}{:016X}",
                          vaddr.GetValue(), static_cast<u64>(data[1]), static_cast<u64>(data[0]));
            },
            [&]() { HandleRasterizerWrite(vaddr.GetValue(), sizeof(u128)); });
        if (ptr)
        {
            return Common::AtomicCompareAndSwap(reinterpret_cast<u64 *>(ptr), data, expected);
        }
        return true;
    }

    struct RasterizerWriteCollectContext
    {
        Impl * impl;
        size_t core;
    };

    static void HandleRasterizerWriteCollectExact(uint64_t address, uint64_t size, void * user_data)
    {
        auto & context = *static_cast<RasterizerWriteCollectContext *>(user_data);
        context.impl->gpu_dirty_managers[context.core].CollectExact(address, size);
    }

    static void HandleRasterizerWriteCollectCoarse(uint64_t address, uint64_t size, void * user_data)
    {
        auto & context = *static_cast<RasterizerWriteCollectContext *>(user_data);
        context.impl->gpu_dirty_managers[context.core].CollectCoarse(address, size);
    }

#if !defined(__APPLE__)
    // Pages holding GPU-written image data that was not downloaded yet
    struct FreeBits
    {
        void operator()(u64 * bits) const
        {
            std::free(bits);
        }
    };

    u64 * ReadCachedBits()
    {
        u64 * bits = read_cached_bits.load(std::memory_order_relaxed);
        if (bits == nullptr)
        {
            const size_t pages = current_page_table->pointers.size();
            bits = static_cast<u64 *>(std::calloc((pages + 63) / 64, sizeof(u64)));
            if (bits == nullptr)
            {
                return nullptr;
            }
            read_cached_storage.reset(bits);
            read_cached_page_count = pages;
            read_cached_bits.store(bits, std::memory_order_release);
        }
        return bits;
    }

    bool IsReadCachedPage(u64 page) const
    {
        u64 * const bits = read_cached_bits.load(std::memory_order_acquire);
        if (bits == nullptr)
        {
            return false;
        }
        if (page >= read_cached_page_count)
        {
            return true;
        }
        return ((std::atomic_ref<u64>(bits[page / 64]).load(std::memory_order_acquire) >> (page % 64)) & 1) != 0;
    }

    bool IsRegionReadCached(u64 vaddr, u64 size) const
    {
        if (read_cached_bits.load(std::memory_order_acquire) == nullptr)
        {
            return false;
        }
        const u64 end = (vaddr + std::max<u64>(size, 1) - 1) >> YUZU_PAGEBITS;
        for (u64 page = vaddr >> YUZU_PAGEBITS; page <= end; ++page)
        {
            if (IsReadCachedPage(page))
            {
                return true;
            }
        }
        return false;
    }

    bool WriteTracking() const
    {
        return current_page_table->write_track_map != nullptr;
    }

    // The JIT only sets the summary levels when the page byte is clear, so an old mark would
    // hide the writes to a page that becomes cached
    void ClearWriteMarks(u64 vaddr, u64 size)
    {
        u8 * const map = current_page_table->write_track_map;
        const u64 end = (vaddr + size + YUZU_PAGEMASK) >> YUZU_PAGEBITS;
        for (u64 page = vaddr >> YUZU_PAGEBITS; page < end; ++page)
        {
            std::atomic_ref<u8> mark(map[page]);
            if (mark.load(std::memory_order_relaxed) != 0)
            {
                mark.store(0, std::memory_order_relaxed);
            }
        }
    }

    // Caller holds read_cached_mutex
    void ProtectTrackedPages(u64 vaddr, u64 size, bool cached)
    {
        if (read_cached_bits.load(std::memory_order_acquire) == nullptr && write_protected_counts.empty())
        {
            return;
        }
        u64 run_begin = 0, run_end = 0;
        Common::MemoryPermission run_perm{};
        const auto flush = [&] {
            if (run_begin != run_end)
            {
                buffer->Protect(run_begin, run_end - run_begin, run_perm);
            }
            run_begin = run_end = 0;
        };
        const u64 end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
        for (u64 page = Common::AlignDown(vaddr, YUZU_PAGESIZE); page < end; page += YUZU_PAGESIZE)
        {
            const u64 index = page >> YUZU_PAGEBITS;
            const bool read_cached = IsReadCachedPage(index);
            if (!read_cached && !write_protected_counts.contains(index))
            {
                flush();
                continue;
            }
            const Common::MemoryPermission perm = !cached       ? Common::MemoryPermission::ReadWrite
                                                  : read_cached ? Common::MemoryPermission{}
                                                                : Common::MemoryPermission::Read;
            if (run_begin != run_end && run_perm != perm)
            {
                flush();
            }
            if (run_begin == run_end)
            {
                run_begin = page;
                run_perm = perm;
            }
            run_end = page + YUZU_PAGESIZE;
        }
        flush();
    }

    // Calls func for each set byte of bytes[begin, end) and clears the byte when it returns true
    template <typename Func>
    static void ScanMarks(u8 * bytes, u64 begin, u64 end, Func && func)
    {
        for (u64 word = begin & ~u64{7}; word < end; word += 8)
        {
            if (std::atomic_ref<u64>(*reinterpret_cast<u64 *>(bytes + word)).load(std::memory_order_relaxed) == 0)
            {
                continue;
            }
            for (u64 index = std::max(word, begin); index < std::min(word + 8, end); ++index)
            {
                std::atomic_ref<u8> mark(bytes[index]);
                if (mark.load(std::memory_order_relaxed) != 0 && func(index))
                {
                    mark.store(0, std::memory_order_relaxed);
                }
            }
        }
    }

    void HarvestCpuWrites()
    {
        if (!WriteTracking())
        {
            return;
        }
        std::scoped_lock lk{harvest_mutex};
        u8 * const map = current_page_table->write_track_map;
        u8 * const l2 = map + current_page_table->write_track_l2_offset;
        u8 * const l1 = map + current_page_table->write_track_l1_offset;
        const u64 pages = current_page_table->pointers.size();
        harvested_pages.clear();
        // A summary byte is cleared before the level below is read, so a write that sets it
        // again is found by the next harvest
        ScanMarks(l1, 0, (pages >> 18) + 1, [&](u64 l1_index) {
            std::atomic_ref<u8>(l1[l1_index]).store(0, std::memory_order_seq_cst);
            ScanMarks(l2, l1_index << 9, std::min((l1_index + 1) << 9, (pages >> 9) + 1), [&](u64 l2_index) {
                std::atomic_ref<u8>(l2[l2_index]).store(0, std::memory_order_seq_cst);
                ScanMarks(map, l2_index << 9, std::min((l2_index + 1) << 9, pages), [&](u64 page) {
                    // Pages that are not cached keep their mark, ClearWriteMarks clears it when they become cached
                    if (current_page_table->pointers[page].Type() != Common::PageType::RasterizerCachedMemory)
                    {
                        return false;
                    }
                    harvested_pages.push_back(page);
                    return true;
                });
                return false;
            });
            return false;
        });
        for (const u64 page : harvested_pages)
        {
            HandleRasterizerWrite(page << YUZU_PAGEBITS, YUZU_PAGESIZE, WriteReport::Coarse);
        }
    }

    // Caller holds read_cached_mutex
    void ProtectCachedRegion(u64 vaddr, u64 size)
    {
        const u64 end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
        u64 begin = Common::AlignDown(vaddr, YUZU_PAGESIZE);
        const auto permission = [&](u64 page) {
            return IsReadCachedPage(page >> YUZU_PAGEBITS) ? Common::MemoryPermission{} : Common::MemoryPermission::Read;
        };
        Common::MemoryPermission current = permission(begin);
        for (u64 page = begin + YUZU_PAGESIZE; page < end; page += YUZU_PAGESIZE)
        {
            const Common::MemoryPermission next = permission(page);
            if (next != current)
            {
                buffer->Protect(begin, page - begin, current);
                begin = page;
                current = next;
            }
        }
        if (begin < end)
        {
            buffer->Protect(begin, end - begin, current);
        }
    }

    void RasterizerMarkRegionReadCached(u64 vaddr, u64 size, s32 delta)
    {
        if (size == 0 || delta == 0 || !ProtectOnlyReadCachedPages() || !AddressSpaceContains(*current_page_table, vaddr, size))
        {
            return;
        }
        std::scoped_lock lk{read_cached_mutex};
        u64 * const bits = ReadCachedBits();
        if (bits == nullptr)
        {
            return;
        }
        const bool protect = current_page_table->fastmem_arena != nullptr;
        u64 run_begin{}, run_end{};
        Common::MemoryPermission run_perm{};
        const auto flush = [&] {
            if (run_begin != run_end)
            {
                buffer->Protect(run_begin, run_end - run_begin, run_perm);
            }
            run_begin = run_end = 0;
        };
        const u64 end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
        for (u64 page = Common::AlignDown(vaddr, YUZU_PAGESIZE); page < end; page += YUZU_PAGESIZE)
        {
            const u64 index = page >> YUZU_PAGEBITS;
            bool was_cached{};
            bool now_cached{};
            std::unordered_map<u64, u32>::iterator it = read_cached_counts.find(index);
            if (delta > 0)
            {
                was_cached = it != read_cached_counts.end();
                if (!was_cached)
                {
                    it = read_cached_counts.emplace(index, 0).first;
                }
                it->second += static_cast<u32>(delta);
                now_cached = true;
            }
            else if (it != read_cached_counts.end())
            {
                was_cached = true;
                if (it->second <= static_cast<u32>(-delta))
                {
                    read_cached_counts.erase(it);
                }
                else
                {
                    it->second -= static_cast<u32>(-delta);
                    now_cached = true;
                }
            }
            if (was_cached == now_cached || index >= read_cached_page_count)
            {
                flush();
                continue;
            }
            std::atomic_ref<u64> word(bits[index / 64]);
            const u64 bit = u64{1} << (index % 64);
            if (now_cached)
            {
                word.fetch_or(bit, std::memory_order_release);
            }
            else
            {
                word.fetch_and(~bit, std::memory_order_release);
            }
            if (protect && current_page_table->pointers[index].Type() == Common::PageType::RasterizerCachedMemory)
            {
                const Common::MemoryPermission perm = now_cached ? Common::MemoryPermission{}
                                                      : WriteTracking() && !write_protected_counts.contains(index)
                                                          ? Common::MemoryPermission::ReadWrite
                                                          : Common::MemoryPermission::Read;
                if (run_begin != run_end && (run_end != page || run_perm != perm))
                {
                    flush();
                }
                if (run_begin == run_end)
                {
                    run_begin = page;
                    run_perm = perm;
                }
                run_end = page + YUZU_PAGESIZE;
            }
            else
            {
                flush();
            }
        }
        flush();
    }

    // A harvested page doesn't tell which of its bytes the CPU wrote, so pages holding
    // GPU-written buffer data deny writes: a write faults and is reported exactly.
    void RasterizerMarkRegionWriteProtected(u64 vaddr, u64 size, s32 delta)
    {
        if (size == 0 || delta == 0 || !WriteTracking() || !AddressSpaceContains(*current_page_table, vaddr, size))
        {
            return;
        }
        std::scoped_lock lk{read_cached_mutex};
        u64 run_begin{}, run_end{};
        Common::MemoryPermission run_perm{};
        const auto flush = [&] {
            if (run_begin != run_end)
            {
                buffer->Protect(run_begin, run_end - run_begin, run_perm);
            }
            run_begin = run_end = 0;
        };
        const u64 end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
        for (u64 page = Common::AlignDown(vaddr, YUZU_PAGESIZE); page < end; page += YUZU_PAGESIZE)
        {
            const u64 index = page >> YUZU_PAGEBITS;
            bool was_protected{};
            bool now_protected{};
            auto it = write_protected_counts.find(index);
            if (delta > 0)
            {
                was_protected = it != write_protected_counts.end();
                if (!was_protected)
                {
                    it = write_protected_counts.emplace(index, 0).first;
                }
                it->second += static_cast<u32>(delta);
                now_protected = true;
            }
            else if (it != write_protected_counts.end())
            {
                was_protected = true;
                if (it->second <= static_cast<u32>(-delta))
                {
                    write_protected_counts.erase(it);
                }
                else
                {
                    it->second -= static_cast<u32>(-delta);
                    now_protected = true;
                }
            }
            // Read-cached pages deny every access already. Uncached pages are protected when they become cached.
            if (was_protected == now_protected || IsReadCachedPage(index) ||
                current_page_table->pointers[index].Type() != Common::PageType::RasterizerCachedMemory)
            {
                flush();
                continue;
            }
            const Common::MemoryPermission perm = now_protected ? Common::MemoryPermission::Read : Common::MemoryPermission::ReadWrite;
            if (run_begin != run_end && run_perm != perm)
            {
                flush();
            }
            if (run_begin == run_end)
            {
                run_begin = page;
                run_perm = perm;
            }
            run_end = page + YUZU_PAGESIZE;
        }
        flush();
    }
#endif

#if defined(__APPLE__)
    // Caller holds nce_write_mutex. Collect permission runs into `out` for batched application,
    // never temporarily exposing GPU-dirty pages while changing buffer write tracking.
    void CollectProtectNceReadCached(u64 vaddr, u64 size,
                                     std::vector<Common::HostMemory::ProtectAccessRange> & out)
    {
        const u64 end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
        u64 cursor = Common::AlignDown(vaddr, YUZU_PAGESIZE);
        if (!osSettings.use_reactive_flushing)
        {
            out.push_back({cursor, end - cursor, Common::MemoryPermission::Read});
            return;
        }
        if (PreciseNceReadTracking())
        {
            u64 begin = cursor;
            auto permission = [&](u64 page) {
                return nce_read_cached.contains(page) && !nce_open_read_pages.contains(page)
                           ? Common::MemoryPermission{}
                           : Common::MemoryPermission::Read;
            };
            auto current = permission(cursor);
            for (u64 page = cursor + YUZU_PAGESIZE; page < end; page += YUZU_PAGESIZE)
            {
                const auto next = permission(page);
                if (next != current)
                {
                    out.push_back({begin, page - begin, current});
                    begin = page;
                    current = next;
                }
            }
            if (begin < end) out.push_back({begin, end - begin, current});
            return;
        }
        auto it = nce_read_cached.lower_bound(cursor);
        while (it != nce_read_cached.end() && it->first < end)
        {
            const u64 begin = it->first;
            u64 run_end = begin;
            do
            {
                run_end += YUZU_PAGESIZE;
                ++it;
            } while (it != nce_read_cached.end() && it->first == run_end && it->first < end);
            if (cursor < begin)
                out.push_back({cursor, begin - cursor, Common::MemoryPermission::Read});
            out.push_back({begin, run_end - begin, Common::MemoryPermission{}});
            cursor = run_end;
        }
        if (cursor < end)
            out.push_back({cursor, end - cursor, Common::MemoryPermission::Read});
    }

    void ProtectNceReadCached(u64 vaddr, u64 size)
    {
        thread_local std::vector<Common::HostMemory::ProtectAccessRange> batch;
        batch.clear();
        CollectProtectNceReadCached(vaddr, size, batch);
        buffer->ProtectAccessBatch(batch);
    }

    void RasterizerMarkRegionReadCached(u64 vaddr, u64 size, s32 delta)
    {
        if (!buffer->UsesNce() || size == 0 || !AddressSpaceContains(*current_page_table, vaddr, size))
            return;
        std::scoped_lock lk{nce_write_mutex};
        const bool reactive = osSettings.use_reactive_flushing;
        thread_local std::vector<Common::HostMemory::ProtectAccessRange> batch;
        batch.clear();
        u64 run_begin{}, run_end{};
        Common::MemoryPermission run_perm{};
        const auto flush = [&] {
            if (run_begin != run_end) batch.push_back({run_begin, run_end - run_begin, run_perm});
            run_begin = run_end = 0;
        };
        const u64 end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
        for (u64 page = Common::AlignDown(vaddr, YUZU_PAGESIZE); page < end; page += YUZU_PAGESIZE)
        {
            auto it = nce_read_cached.find(page);
            bool skip_protect = false;
            if (delta > 0)
            {
                const bool was_present = it != nce_read_cached.end();
                const bool was_direct_read = was_present && it->second.direct_read;
                const bool was_open = nce_open_read_pages.erase(page) != 0;
                if (!was_present) it = nce_read_cached.emplace(page, NceReadCachedEntry{}).first;
                it->second.count += static_cast<u32>(delta);
                ++it->second.generation;
                it->second.direct_read = false;
                it->second.neighbor_defer_until_ns = 0;
                // Already read-cached, not opened for native/direct reads or writes, and
                // confirmed read-denied in NceDirectMap: host protection is already PROT_NONE.
                if (reactive && was_present && !was_open && !was_direct_read &&
                    !nce_written.contains(page) && !buffer->NceTrackedAccessAllowed(page, false))
                {
                    skip_protect = true;
                }
            }
            else if (it != nce_read_cached.end())
            {
                if (it->second.count <= static_cast<u32>(-delta))
                {
                    nce_read_cached.erase(it);
                    nce_open_read_pages.erase(page);
                    it = nce_read_cached.end();
                }
                else
                {
                    it->second.count -= static_cast<u32>(-delta);
                    // Still read-cached: protection state did not change.
                    skip_protect = true;
                }
            }
            if (skip_protect)
            {
                flush();
                continue;
            }
            const auto page_type = current_page_table->pointers[page >> YUZU_PAGEBITS].Type();
            if (page_type == Common::PageType::RasterizerCachedMemory ||
                page_type == Common::PageType::Memory ||
                page_type == Common::PageType::DebugMemory)
            {
                const bool protect = it != nce_read_cached.end() && reactive;
                const auto default_perm =
                    page_type == Common::PageType::DebugMemory
                        ? Common::MemoryPermission{}
                        : (page_type == Common::PageType::RasterizerCachedMemory
                               ? Common::MemoryPermission::Read
                               : Common::MemoryPermission::ReadWrite);
                const auto perm = protect ? Common::MemoryPermission{} : default_perm;
                if (run_begin != run_end && (run_end != page || run_perm != perm)) flush();
                if (run_begin == run_end)
                {
                    run_begin = page;
                    run_perm = perm;
                }
                run_end = page + YUZU_PAGESIZE;
            }
            else
                flush();
        }
        flush();
        if (!batch.empty())
        {
            buffer->ProtectAccessBatch(batch);
        }
    }

    bool DemandNceReadback() const
    {
        static const bool enabled = std::getenv("NXEMU_NCE_DEMAND_READBACK") != nullptr;
        return enabled && buffer->UsesNce();
    }

    bool PreciseNceReadTracking() const
    {
        static const bool enabled = std::getenv("NXEMU_NCE_PRECISE_READ_TRACKING") != nullptr;
        return enabled && DemandNceReadback();
    }

    void SetNativeReadAccess(u64 vaddr, u64 size, bool readable)
    {
        if (!PreciseNceReadTracking() || size == 0 ||
            !AddressSpaceContains(*current_page_table, vaddr, size)) return;
        std::scoped_lock lk{nce_write_mutex};
        u64 run_begin{}, run_end{};
        Common::MemoryPermission run_perm{};
        const auto flush = [&] {
            if (run_begin != run_end)
            {
                buffer->ProtectAccess(run_begin, run_end - run_begin, run_perm);
            }
            run_begin = run_end = 0;
        };
        const u64 end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
        for (u64 page = Common::AlignDown(vaddr, YUZU_PAGESIZE); page < end; page += YUZU_PAGESIZE)
        {
            const auto page_type = current_page_table->pointers[page >> YUZU_PAGEBITS].Type();
            const bool cached = page_type == Common::PageType::RasterizerCachedMemory;
            const bool tracked = cached && nce_read_cached.contains(page);
            if (readable)
            {
                if (tracked)
                {
                    nce_open_read_pages.insert(page);
                    has_nce_open_read_pages.store(true, std::memory_order_release);
                }
                const auto perm =
                    page_type == Common::PageType::DebugMemory
                        ? Common::MemoryPermission{}
                        : (cached ? Common::MemoryPermission::Read
                                  : Common::MemoryPermission::ReadWrite);
                if (run_begin != run_end && (run_end != page || run_perm != perm)) flush();
                if (run_begin == run_end)
                {
                    run_begin = page;
                    run_perm = perm;
                }
                run_end = page + YUZU_PAGESIZE;
            }
            else
            {
                nce_open_read_pages.erase(page);
                if (nce_open_read_pages.empty())
                {
                    has_nce_open_read_pages.store(false, std::memory_order_release);
                }
                if (tracked)
                {
                    const auto perm = Common::MemoryPermission{};
                    if (run_begin != run_end && (run_end != page || run_perm != perm)) flush();
                    if (run_begin == run_end)
                    {
                        run_begin = page;
                        run_perm = perm;
                    }
                    run_end = page + YUZU_PAGESIZE;
                }
                else
                {
                    flush();
                }
            }
        }
        flush();
    }

    void CollectRearmNceReads(std::vector<Common::HostMemory::ProtectAccessRange> & out)
    {
        if (nce_open_read_pages.empty()) return;
        if (!osSettings.use_reactive_flushing)
        {
            nce_open_read_pages.clear();
            return;
        }
        u64 begin{}, end{};
        const auto flush = [&] {
            if (begin != end) out.push_back({begin, end - begin, Common::MemoryPermission{}});
            begin = end = 0;
        };
        for (const u64 page : nce_open_read_pages)
        {
            const auto page_type = current_page_table->pointers[page >> YUZU_PAGEBITS].Type();
            if (!nce_read_cached.contains(page) || page_type == Common::PageType::Unmapped)
            {
                flush();
                continue;
            }
            if (begin != end && end != page) flush();
            if (begin == end) begin = page;
            end = page + YUZU_PAGESIZE;
        }
        flush();
        nce_open_read_pages.clear();
    }

    // Only the guest page the CPU reads is a guest read. A store, or a page that merely
    // shares the 16 KiB host page, needs current memory before the page opens, but must not
    // make its images readback targets: their later writebacks would overwrite memory the
    // guest has reused.
    static bool IsSharedNcePage(u64 begin, u64 end, [[maybe_unused]] u64 vaddr, [[maybe_unused]] bool read)
    {
#if defined(__APPLE__)
        return !(read && begin <= vaddr && vaddr < end);
#else
        return false;
#endif
    }

    void DownloadNceHostPage(u64 vaddr, bool read = true)
    {
        struct ReadCachedSnapshot
        {
            u64 page{};
            u32 generation{};
            bool direct_read{};
        };
        std::array<ReadCachedSnapshot, NceHostPageSize / YUZU_PAGESIZE> pages{};
        size_t count{};
        const u64 host_page = vaddr & ~(NceHostPageSize - 1);
        const u64 target_page = vaddr & ~YUZU_PAGEMASK;
        bool target_is_readback_page = false;
        const bool precise = PreciseNceReadTracking();
        const bool demand = DemandNceReadback();
        u64 now_ns = 0;
        const auto get_now_ns = [&]() -> u64 {
            if (now_ns == 0)
            {
                now_ns = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
            }
            return now_ns;
        };
        {
            std::scoped_lock lk{nce_write_mutex};
            if (!osSettings.use_reactive_flushing) return;
            target_is_readback_page =
                read && (nce_read_cached.contains(target_page) ||
                         nce_ever_direct_read_pages.contains(target_page));
            for (auto it = nce_read_cached.lower_bound(host_page);
                 it != nce_read_cached.end() && it->first < host_page + NceHostPageSize; ++it)
            {
                const bool is_target_page = (it->first == target_page);
                // A 4 KiB page already readable in NceDirectMap needs no repeat download
                // unless it was previously opened only as a shared neighbor and is now being
                // directly read (so it promotes to a readback target once).
                const bool tracked_readable = buffer->NceTrackedAccessAllowed(it->first, false);
                if (tracked_readable && (!read || !is_target_page || it->second.direct_read))
                    continue;
                if (!is_target_page)
                {
                    if (target_is_readback_page && !precise && !demand && !it->second.direct_read)
                        continue;
                    if (read && it->second.neighbor_defer_until_ns != 0)
                    {
                        if (get_now_ns() < it->second.neighbor_defer_until_ns) continue;
                        it->second.neighbor_defer_until_ns = 0;
                    }
                }
                // A different 4 KiB neighbor may have trapped this 16 KiB host page.
                // Pages already synchronized remain readable until a real GPU write
                // revokes them on the producer; do not download them again here.
                if (!precise || !nce_open_read_pages.contains(it->first))
                    pages[count++] = {it->first, it->second.generation, it->second.direct_read};
            }
        }
        if (count == 0) return;
        // GPU readback may untrack textures and acquire nce_write_mutex. Do not hold it
        // while synchronizing. Read every protected neighbor before opening the host page.
        std::array<u64, NceHostPageSize / YUZU_PAGESIZE> deferred{};
        size_t deferred_count{};
        std::array<u64, NceHostPageSize / YUZU_PAGESIZE> neighbor_deferred{};
        size_t neighbor_deferred_count{};
        for (size_t i = 0; i < count;)
        {
            const auto & first = pages[i++];
            const u64 begin = first.page;
            u64 end = begin + YUZU_PAGESIZE;
            const auto pointer = reinterpret_cast<uintptr_t>(GetPointerFromRasterizerCachedMemory(begin));
            // Adjacent guest pages may map unrelated physical pages. Merge only when
            // their CPU backing is also contiguous.
            while (demand && i < count && pages[i].page == end && pointer != 0 &&
                   reinterpret_cast<uintptr_t>(GetPointerFromRasterizerCachedMemory(pages[i].page)) ==
                       pointer + end - begin)
            {
                end += YUZU_PAGESIZE;
                ++i;
            }
            const bool neighbor = !(begin <= vaddr && vaddr < end);
            bool defer = false;
            if (neighbor && target_is_readback_page && !precise && !demand && !first.direct_read)
            {
                // The CPU is actively reading a GPU readback page on this 16 KiB host page,
                // and a neighbouring 4 KiB page holds another GPU-modified target that has
                // never been directly read. Keep the neighbour closed so a subsequent read
                // of it faults directly and promotes it to normal async readback instead of
                // reading stale guest memory (e.g. double-buffered render targets spanning
                // a 4 KiB boundary inside one 16 KiB Apple Silicon host page).
                defer = true;
            }
            else
            {
                const bool shared = IsSharedNcePage(begin, end, vaddr, read) && !precise;
                // Opening the host page would expose data an asynchronous copy has not
                // published. The access is emulated instead until that copy completes.
                defer = HandleRasterizerDownload(begin, end - begin, shared, /*from_nce_fault=*/true);
                // An existing readback target may still have its asynchronous copy in flight,
                // and the guest can read the page before the fence that publishes it.
                if (!shared && !demand)
                    defer = HandleRasterizerDownload(begin, end - begin, true, /*from_nce_fault=*/true) || defer;
                if (defer && neighbor && read)
                {
                    for (u64 page = begin; page < end; page += YUZU_PAGESIZE)
                        neighbor_deferred[neighbor_deferred_count++] = page;
                }
            }
            for (u64 page = begin; defer && page < end; page += YUZU_PAGESIZE)
                deferred[deferred_count++] = page;
        }
        // Precise mode grants access and registers the observer on the GPU producer
        // before it resumes drawing. Reopening here could undo a subsequent GPU write.
        if (precise) return;
        const u64 neighbor_backoff_until_ns =
            neighbor_deferred_count != 0 ? (get_now_ns() + 2'000'000ULL) : 0;
        std::scoped_lock lk{nce_write_mutex};
        u64 run_begin{}, run_end{};
        Common::MemoryPermission run_perm{};
        const auto flush_protect = [&] {
            if (run_begin != run_end)
            {
                buffer->ProtectAccess(run_begin, run_end - run_begin, run_perm);
            }
            run_begin = run_end = 0;
        };
        for (size_t i = 0; i < count; ++i)
        {
            const u64 page = pages[i].page;
            const bool cached = current_page_table->pointers[page >> YUZU_PAGEBITS].Type() ==
                                Common::PageType::RasterizerCachedMemory;
            // A shared page closes again at the next flush, so that a later guest read of
            // it still downloads current data.
            const bool shared = IsSharedNcePage(page, page + YUZU_PAGESIZE, vaddr, read);
            if (std::find(deferred.begin(), deferred.begin() + deferred_count, page) !=
                deferred.begin() + deferred_count)
            {
                if (neighbor_backoff_until_ns != 0 &&
                    std::find(neighbor_deferred.begin(),
                              neighbor_deferred.begin() + neighbor_deferred_count,
                              page) != neighbor_deferred.begin() + neighbor_deferred_count)
                {
                    const auto cached_it = nce_read_cached.find(page);
                    if (cached_it != nce_read_cached.end() &&
                        cached_it->second.generation == pages[i].generation)
                    {
                        cached_it->second.neighbor_defer_until_ns = neighbor_backoff_until_ns;
                    }
                }
                flush_protect();
                continue;
            }
            if (read && page == target_page)
            {
                nce_ever_direct_read_pages.insert(page);
            }
            const auto cached_it = nce_read_cached.find(page);
            if (cached_it != nce_read_cached.end())
            {
                if (cached_it->second.generation != pages[i].generation)
                {
                    // A new GPU write entered this page while we synchronized; keep it protected.
                    flush_protect();
                    continue;
                }
                cached_it->second.neighbor_defer_until_ns = 0;
                if (read && page == target_page)
                {
                    cached_it->second.direct_read = true;
                }
                if (demand || shared)
                {
                    nce_open_read_pages.insert(page);
                    has_nce_open_read_pages.store(true, std::memory_order_release);
                }
            }
            const auto perm = cached ? Common::MemoryPermission::Read
                                     : Common::MemoryPermission::ReadWrite;
            if (run_begin != run_end && (run_end != page || run_perm != perm)) flush_protect();
            if (run_begin == run_end)
            {
                run_begin = page;
                run_perm = perm;
            }
            run_end = page + YUZU_PAGESIZE;
        }
        flush_protect();
    }

    bool NceTrackWrites(u64 vaddr, bool canonical = false)
    {
        std::unique_lock lk{nce_write_mutex};
        const u64 host_page = vaddr & ~(NceHostPageSize - 1);
        const u64 target_page = vaddr & ~YUZU_PAGEMASK;
        if (canonical)
        {
            if (!buffer->NceAccessAllowed(vaddr, true)) return false;
            WaitForActiveGpuReadbacksForVAddrLocked(lk, target_page, target_page + YUZU_PAGESIZE);
            const bool cached = current_page_table->pointers[target_page >> YUZU_PAGEBITS].Type() ==
                                Common::PageType::RasterizerCachedMemory;
            // A canonical store touches only this 4 KiB page. A dirty neighbour
            // cannot require another readback/snapshot for an already owned page.
            if (buffer->NceTrackedAccessAllowed(vaddr, false) &&
                buffer->NceTrackedAccessAllowed(vaddr, true) &&
                (!cached || nce_written.contains(target_page))) return true;
        }
        // A host page containing texture data must retain reactive read protection. Other
        // cached pages keep the snapshot path; opening them cannot expose a stale texture.
        // For canonical stores, only the single 4 KiB target_page is opened in atomic_table.
        if (osSettings.use_reactive_flushing)
        {
            const u64 check_begin = canonical ? target_page : host_page;
            const u64 check_end = canonical ? target_page + YUZU_PAGESIZE : host_page + NceHostPageSize;
            const auto it = nce_read_cached.lower_bound(check_begin);
            if (it != nce_read_cached.end() && it->first < check_end)
            {
                lk.unlock();
                DownloadNceHostPage(vaddr, false);
                lk.lock();
                if (!(canonical ? buffer->NceTrackedAccessAllowed(vaddr, false)
                                : buffer->NceHostAccessAllowed(vaddr, false))) return false;
            }
        }
        // The GPU cache may have released the page since the fault.
        const u64 begin = canonical ? target_page : host_page;
        const u64 end = canonical ? target_page + YUZU_PAGESIZE : host_page + NceHostPageSize;
        // Never snapshot or open a page while an unlocked GPU readback is writing to its
        // physical backing (on this VA or any alias).
        WaitForActiveGpuReadbacksForVAddrLocked(lk, begin, end);
        bool opened = canonical ? buffer->NceTrackedAccessAllowed(vaddr, true)
                                : buffer->NceHostAccessAllowed(vaddr, true);
        u64 protect_begin{}, protect_end{};
        const auto flush_rw = [&] {
            if (protect_begin != protect_end)
            {
                buffer->ProtectAccess(protect_begin, protect_end - protect_begin,
                                      Common::MemoryPermission::ReadWrite);
            }
            protect_begin = protect_end = 0;
        };
        for (u64 page = begin; page < end; page += YUZU_PAGESIZE)
        {
            if (current_page_table->pointers[page >> YUZU_PAGEBITS].Type() !=
                Common::PageType::RasterizerCachedMemory)
            {
                flush_rw();
                continue;
            }
            const u8 * const pointer = GetPointerFromRasterizerCachedMemory(page);
            if (pointer == nullptr)
            {
                flush_rw();
                continue;
            }
            auto [it, inserted] = nce_written.try_emplace(page);
            if (inserted)
            {
                // The snapshot is completely overwritten before the page becomes writable.
                const uintptr_t phys_page = PhysPageOf(pointer);
                it->second.copy = AcquirePageCopy();
                it->second.phys_page = phys_page;
                std::memcpy(it->second.copy->data(), pointer, YUZU_PAGESIZE);
                InsertPhysEntry(nce_written_by_phys, phys_page, page);
                has_nce_written.store(true, std::memory_order_release);
            }
            // Readback may have write-protected a page already in nce_written. Restore
            // access for existing snapshots too, otherwise retrying the store loops forever.
            const bool needs_rw =
                inserted || !(canonical ? buffer->NceTrackedAccessAllowed(page, true)
                                        : buffer->NceHostAccessAllowed(vaddr, true));
            if (needs_rw)
            {
                if (protect_begin != protect_end && protect_end != page) flush_rw();
                if (protect_begin == protect_end) protect_begin = page;
                protect_end = page + YUZU_PAGESIZE;
            }
            else
                flush_rw();
            opened = true;
        }
        flush_rw();
        // A fragmented or read-only guest mapping can still forbid a native store.
        // Such accesses must be interpreted rather than retried without progress.
        if (canonical)
        {
            return buffer->NceTrackedAccessAllowed(vaddr, false) &&
                   buffer->NceTrackedAccessAllowed(vaddr, true);
        }
        return opened && buffer->NceHostAccessAllowed(vaddr, true);
    }

    // Zero-copy fast path for GPU readbacks (e.g. block-linear texture swizzle): executes
    // fn(dst, size, user) in place without holding global nce_write_mutex across multi-MiB
    // swizzles, using active_gpu_readbacks to exclude only overlapping CPU write opens.
    bool TryRunInPlaceGpuReadback(u64 vaddr, u8 * dst, u64 size,
                                  void (*fn)(u8 *, u64, void *), void * user)
    {
        if (size == 0)
        {
            return true;
        }
        if (!buffer->UsesNce())
        {
            fn(dst, size, user);
            return true;
        }
        if (vaddr + size < vaddr || !AddressSpaceContains(*current_page_table, vaddr, size))
        {
            return false;
        }
        const uintptr_t dst_begin = PhysPageOf(dst);
        const uintptr_t dst_byte_end = reinterpret_cast<uintptr_t>(dst) + size;
        const uintptr_t dst_end = Common::AlignUp(dst_byte_end, YUZU_PAGESIZE);
        const u64 page_begin = vaddr & ~YUZU_PAGEMASK;
        const u64 page_end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);

        std::unique_lock lk{nce_write_mutex};
        // Check physical-page maps so multi-VA aliases with pending CPU writes are caught too.
        if (has_nce_written.load(std::memory_order_relaxed))
        {
            const auto it = LowerBoundPhys(nce_written_by_phys, dst_begin);
            if (it != nce_written_by_phys.end() && it->first < dst_byte_end)
            {
                return false;
            }
        }
        if (has_nce_flushing.load(std::memory_order_relaxed))
        {
            const auto it = LowerBoundPhys(nce_flushing_by_phys, dst_begin);
            if (it != nce_flushing_by_phys.end() && it->first < dst_byte_end)
            {
                return false;
            }
        }
        // Verify every target page is currently write-protected (RasterizerCachedMemory & !W)
        // so live native CPU stores cannot race with the in-place swizzle.
        const auto * const atomic_table =
            static_cast<const std::atomic<u64> *>(buffer->GetNceAtomicTable());
        const bool fast_atomic = atomic_table != nullptr && page_end <= (1ULL << 39);
        for (u64 page = page_begin; page < page_end; page += YUZU_PAGESIZE)
        {
            const size_t idx = page >> YUZU_PAGEBITS;
            if (current_page_table->pointers[idx].Type() !=
                Common::PageType::RasterizerCachedMemory)
            {
                return false;
            }
            const bool writable =
                fast_atomic
                    ? (atomic_table[idx].load(std::memory_order_acquire) & 2ULL) != 0
                    : buffer->NceTrackedAccessAllowed(page, true);
            if (writable)
            {
                return false;
            }
        }
        active_gpu_readbacks.emplace_back(dst_begin, dst_end);
        lk.unlock();

        fn(dst, size, user);

        lk.lock();
        RemoveActiveGpuReadbackLocked(dst_begin, dst_end);
        return true;
    }

    // A native store reaches guest memory before FlushNceWrites reports it to the GPU caches,
    // which only then cancel the downloads it overlaps. Until that report completes in
    // CompleteNceWriteFlush, a GPU readback must not overwrite the store, and its own bytes
    // must not look like CPU writes.
    bool WriteGpuReadback(u64 /*vaddr*/, u8 * dst, const u8 * src, u64 size,
                          void (*copy)(u8 *, const u8 *, u64, void *), void * user)
    {
        if (!buffer->UsesNce()) return false;
        if (size == 0) return true;
        const uintptr_t base = reinterpret_cast<uintptr_t>(dst);
        const uintptr_t dst_begin = PhysPageOf(dst);
        const uintptr_t dst_byte_end = base + size;
        const uintptr_t dst_end = Common::AlignUp(dst_byte_end, YUZU_PAGESIZE);

        std::unique_lock lk{nce_write_mutex};
        const bool any_written = has_nce_written.load(std::memory_order_relaxed);
        const bool any_flushing = has_nce_flushing.load(std::memory_order_relaxed);
        auto it_w = any_written ? LowerBoundPhys(nce_written_by_phys, dst_begin)
                                : nce_written_by_phys.end();
        auto it_f = any_flushing ? LowerBoundPhys(nce_flushing_by_phys, dst_begin)
                                 : nce_flushing_by_phys.end();
        const bool hit_w = it_w != nce_written_by_phys.end() && it_w->first < dst_byte_end;
        const bool hit_f = it_f != nce_flushing_by_phys.end() && it_f->first < dst_byte_end;
        if (!hit_w && !hit_f)
        {
            // Fast path: no overlapping dirty pages (on any VA alias) -> run copy without
            // holding global nce_write_mutex across multi-MiB transfers.
            active_gpu_readbacks.emplace_back(dst_begin, dst_end);
            lk.unlock();

            copy(dst, src, size, user);

            lk.lock();
            RemoveActiveGpuReadbackLocked(dst_begin, dst_end);
            return true;
        }

        // Register the full physical range in active_gpu_readbacks so clean-gap copies can
        // release nce_write_mutex without allowing concurrent NCE write faults to open a
        // clean page inside [dst_begin, dst_end).
        active_gpu_readbacks.emplace_back(dst_begin, dst_end);
        SCOPE_EXIT {
            if (!lk.owns_lock())
            {
                lk.lock();
            }
            RemoveActiveGpuReadbackLocked(dst_begin, dst_end);
        };

        uintptr_t cursor = base;
        std::vector<u8 *> refs;
        std::vector<u64> dirty_open_vaddrs;
        while (cursor < dst_byte_end)
        {
            while (it_w != nce_written_by_phys.end() && it_w->first + YUZU_PAGESIZE <= cursor)
                ++it_w;
            while (it_f != nce_flushing_by_phys.end() && it_f->first + YUZU_PAGESIZE <= cursor)
                ++it_f;

            const uintptr_t next_w = (it_w != nce_written_by_phys.end() && it_w->first < dst_byte_end)
                                         ? std::max(it_w->first, cursor)
                                         : dst_byte_end;
            const uintptr_t next_f = (it_f != nce_flushing_by_phys.end() && it_f->first < dst_byte_end)
                                         ? std::max(it_f->first, cursor)
                                         : dst_byte_end;
            const uintptr_t next = std::min(next_w, next_f);
            if (cursor < next)
            {
                const u64 gap_off = cursor - base;
                const u64 gap_size = next - cursor;
                if (gap_size >= YUZU_PAGESIZE)
                {
                    lk.unlock();
                    copy(dst + gap_off, src + gap_off, gap_size, user);
                    lk.lock();
                    it_w = has_nce_written.load(std::memory_order_relaxed)
                               ? LowerBoundPhys(nce_written_by_phys, next)
                               : nce_written_by_phys.end();
                    it_f = has_nce_flushing.load(std::memory_order_relaxed)
                               ? LowerBoundPhys(nce_flushing_by_phys, next)
                               : nce_flushing_by_phys.end();
                    cursor = next;
                    continue;
                }
                copy(dst + gap_off, src + gap_off, gap_size, user);
                cursor = next;
            }
            if (cursor == dst_byte_end) break;

            const uintptr_t phys_page = cursor & ~YUZU_PAGEMASK;
            const uintptr_t page_end = std::min<uintptr_t>(phys_page + YUZU_PAGESIZE, dst_byte_end);
            const u64 chunk_off = cursor - phys_page;
            const u64 chunk_size = page_end - cursor;

            refs.clear();
            dirty_open_vaddrs.clear();
            while (it_w != nce_written_by_phys.end() && it_w->first == phys_page)
            {
                const u64 va = it_w->second;
                if (buffer->NceTrackedAccessAllowed(va, true))
                {
                    buffer->ProtectAccess(va, YUZU_PAGESIZE, Common::MemoryPermission::Read);
                    dirty_open_vaddrs.push_back(va);
                }
                refs.push_back(nce_written.at(va).copy->data());
                ++it_w;
            }
            while (it_f != nce_flushing_by_phys.end() && it_f->first == phys_page)
            {
                refs.push_back(nce_flushing.at(it_f->second).copy->data());
                ++it_f;
            }
            u8 * const dst_chunk = dst + (cursor - base);
            u8 * const page_dst = dst_chunk - chunk_off;
            const u8 * const src_chunk = src + (cursor - base);
            if (refs.size() == 1)
            {
                Core::MergeNceReadbackWords(refs[0], page_dst, chunk_off, src_chunk, chunk_size);
            }
            else if (refs.size() > 1)
            {
                Core::MergeNceReadbackWordsMulti(refs.data(), refs.size(), page_dst, chunk_off,
                                                 src_chunk, chunk_size);
            }
            else
            {
                copy(dst_chunk, src_chunk, chunk_size, user);
            }
            for (const u64 va : dirty_open_vaddrs)
            {
                buffer->ProtectAccess(va, YUZU_PAGESIZE, Common::MemoryPermission::ReadWrite);
            }
            cursor = page_end;
        }
        return true;
    }

    void FlushNceWrites(bool rearm_native_reads = true, bool defer_release = false)
    {
        // Lock-free fast path when no pages have pending writes or open reads.
        if (!has_nce_written.load(std::memory_order_acquire) &&
            (!rearm_native_reads || !has_nce_open_read_pages.load(std::memory_order_acquire)))
        {
            return;
        }
        thread_local std::vector<std::pair<u64, u64>> written;
        written.clear();
        {
            std::scoped_lock lk{nce_write_mutex};
            // Pages unmapped since are not cached anymore.
            for (auto it = nce_written.begin(); it != nce_written.end();)
            {
                if (current_page_table->pointers[it->first >> YUZU_PAGEBITS].Type() !=
                    Common::PageType::RasterizerCachedMemory)
                {
                    ErasePhysEntry(nce_written_by_phys, it->second.phys_page, it->first);
                    ReleasePageCopy(std::move(it->second.copy));
                    it = nce_written.erase(it);
                }
                else
                    ++it;
            }
            // Collect all protection changes (open reads + written pages) into a single
            // batched SetAccessMask call to take NceDirectMap::mutex once, deduplicate shared
            // 16 KiB host pages, and coalesce adjacent mach_vm_protect syscalls.
            thread_local std::vector<Common::HostMemory::ProtectAccessRange> protect_batch;
            protect_batch.clear();
            // Only pages opened by a native read are rearmed; untouched image pages
            // retain their existing protection without a per-image walk.
            if (rearm_native_reads && !PreciseNceReadTracking())
            {
                CollectRearmNceReads(protect_batch);
                if (nce_open_read_pages.empty())
                {
                    has_nce_open_read_pages.store(false, std::memory_order_release);
                }
            }
            // Protect before comparing, so that later writes fault again.
            for (auto it = nce_written.begin(); it != nce_written.end();)
            {
                const u64 begin = it->first;
                u64 end = begin;
                while (it != nce_written.end() && it->first == end)
                {
                    end += YUZU_PAGESIZE;
                    ++it;
                }
                if (rearm_native_reads)
                    CollectProtectNceReadCached(begin, end - begin, protect_batch);
                else
                    protect_batch.push_back({begin, end - begin, Common::MemoryPermission::Read});
            }
            if (!protect_batch.empty())
            {
                buffer->ProtectAccessBatch(protect_batch);
            }
            // Report only the bytes the CPU changed in a single NEON-accelerated pass.
            // The rest of the page can hold GPU writes that are not downloaded yet,
            // invalidating it would upload stale guest memory.
            for (auto & [page, entry] : nce_written)
            {
                const size_t before = written.size();
                const u8 * const pointer = GetPointerFromRasterizerCachedMemory(page);
                if (pointer != nullptr)
                {
                    ForEachNceWrittenRange(pointer, entry.copy->data(), YUZU_PAGESIZE,
                                           [&](u64 offset, u64 size) {
                                               written.emplace_back(page + offset, size);
                                           });
                }
                if (defer_release && written.size() != before)
                {
                    // Keep the snapshot visible to concurrent GPUFencingThread readbacks
                    // until GatherGPUDirtyMemory finishes cancelling overlapping downloads.
                    // If already present in nce_flushing, preserve the earlier baseline snapshot.
                    auto [it, inserted] = nce_flushing.try_emplace(page);
                    if (inserted)
                    {
                        const uintptr_t phys_page = entry.phys_page;
                        it->second = std::move(entry);
                        InsertPhysEntry(nce_flushing_by_phys, phys_page, page);
                    }
                    else
                    {
                        ReleasePageCopy(std::move(entry.copy));
                    }
                }
                else
                {
                    ReleasePageCopy(std::move(entry.copy));
                }
            }
            nce_written.clear();
            nce_written_by_phys.clear();
            has_nce_written.store(false, std::memory_order_release);
            if (!nce_flushing.empty())
            {
                has_nce_flushing.store(true, std::memory_order_release);
            }
        }
        if (written.empty())
        {
            return;
        }
        constexpr size_t sys_core = Hardware::NUM_CPU_CORES - 1;
        const size_t core = std::min(system.GetCurrentHostThreadID(), sys_core);
        std::unique_lock sys_lock{sys_core_guard, std::defer_lock};
        if (core == sys_core) [[unlikely]]
        {
            sys_lock.lock();
        }
        struct FlushNceCollectContext
        {
            Impl * impl;
            size_t core;
        };
        const auto collect_exact = [](uint64_t address, uint64_t size, void * user_data) {
            auto & ctx = *static_cast<FlushNceCollectContext *>(user_data);
            ctx.impl->gpu_dirty_managers[ctx.core].CollectExact(address, size);
        };
        FlushNceCollectContext context{this, core};
        // Report every contiguous run of CPU-written words without merging across unchanged
        // words: TextureCache::WriteMemory and BufferCache::IsRegionGpuModified are byte-granular,
        // so bridging across unchanged words can invalidate a GPU-modified image or buffer.
        for (const auto & [address, size] : written)
        {
            if (const auto * p = GetPointerImpl(address, []() {}, []() {}); p != nullptr)
            {
                system.GetVideo().HandleRasterizerWrite(
                    p, size, &rasterizer_write_areas[core].last_address, collect_exact,
                    &context);
            }
        }
    }

    void CompleteNceWriteFlush()
    {
        if (!has_nce_flushing.load(std::memory_order_acquire))
        {
            return;
        }
        std::scoped_lock lk{nce_write_mutex};
        for (auto & [page, entry] : nce_flushing)
        {
            ReleasePageCopy(std::move(entry.copy));
        }
        nce_flushing.clear();
        nce_flushing_by_phys.clear();
        has_nce_flushing.store(false, std::memory_order_release);
    }
#endif

    // Returns true when a query result the read covers is not written yet. With kick, the
    // GPU work that writes it is submitted.
    bool AwaitPendingResult(VAddr v_address, size_t size, bool kick)
    {
        const auto * p = GetPointerImpl(v_address, []() {}, []() {});
        if (p == nullptr) return false;
        RasterizerDownloadArea request{};
        request.pending_result = true;
        request.kick = kick;
        return system.GetVideo().HandleRasterizerDownload(p, size, request).deferred;
    }

    // Returns true when a shared page must stay inaccessible until an asynchronous copy
    // publishes it.
    bool HandleRasterizerDownload(VAddr v_address, size_t size, [[maybe_unused]] bool shared_page = false,
                                  [[maybe_unused]] bool from_nce_fault = false)
    {
        const auto * p = GetPointerImpl(v_address, []() {}, []() {});
        if (p == nullptr)
        {
            return false;
        }
#if defined(__APPLE__)
        if (shared_page)
        {
            RasterizerDownloadArea request{};
            request.native_read = true;
            request.shared_page = true;
            return system.GetVideo().HandleRasterizerDownload(p, size, request).deferred;
        }
        if (DemandNceReadback())
        {
            // Do not reuse a whole-image range across GPU submissions. It carries no
            // generation and could otherwise suppress a required native readback.
            RasterizerDownloadArea request{};
            request.native_read = true;
            system.GetVideo().HandleRasterizerDownload(p, size, request);
            return false;
        }
        if (from_nce_fault)
        {
            // An NCE page fault only fires when a page was newly read-protected by the GPU
            // (for example, a second render target sharing the same 4 KiB page as an earlier
            // readback). Reusing rasterizer_read_areas[core] across submissions would suppress
            // OnCPURead and leave the new target un-downloaded and un-registered for preemptive
            // readback.
            RasterizerDownloadArea area{};
            (void)system.GetVideo().HandleRasterizerDownload(p, size, area);
            return false;
        }
#else
        if (ProtectOnlyReadCachedPages() && !IsRegionReadCached(v_address, size))
        {
            return false;
        }
#endif
        const size_t core = system.GetCurrentHostThreadID();
        if (core < Hardware::NUM_CPU_CORES) [[likely]]
        {
            RasterizerDownloadArea & area = rasterizer_read_areas[core];
            area = system.GetVideo().HandleRasterizerDownload(p, size, area);
        }
        else
        {
            // Auxiliary host threads have IDs outside the CPU-core range. Give each its
            // own cache, avoiding both out-of-bounds access and cross-thread contention.
            struct ThreadReadbackArea
            {
                u64 generation{};
                RasterizerDownloadArea area{};
            };
            static thread_local ThreadReadbackArea thread_readback{};
            if (thread_readback.generation != rasterizer_readback_generation)
            {
                thread_readback.area = {};
                thread_readback.generation = rasterizer_readback_generation;
            }
            thread_readback.area =
                system.GetVideo().HandleRasterizerDownload(p, size, thread_readback.area);
        }
        return false;
    }

    enum class WriteReport
    {
        Exact,
        Coarse,
    };

    // Exact even for small writes: a 64-byte block around one can hold GPU-written bytes, which
    // a widened report would replace with stale guest memory
    void HandleRasterizerWrite(VAddr v_address, uint64_t size, WriteReport report = WriteReport::Exact)
    {
        const auto * p = GetPointerImpl(v_address, []() {}, []() {});
        if (p == nullptr)
        {
            return;
        }
        constexpr size_t sys_core = Hardware::NUM_CPU_CORES - 1;
        const size_t core = std::min(system.GetCurrentHostThreadID(), sys_core);

        if (core == sys_core) [[unlikely]]
        {
            sys_core_guard.lock();
        }
        SCOPE_EXIT
        {
            if (core == sys_core) [[unlikely]]
            {
                sys_core_guard.unlock();
            }
        };

        RasterizerWriteCollectContext context{this, core};
        if (report == WriteReport::Coarse)
        {
            system.GetVideo().HandleRasterizerCoarseWrite(p, size, HandleRasterizerWriteCollectCoarse, &context);
            return;
        }
        system.GetVideo().HandleRasterizerWrite(p, size, &rasterizer_write_areas[core].last_address,
                                                HandleRasterizerWriteCollectExact, &context);
    }

    struct GPUDirtyState
    {
        uint64_t last_address;
    };

    void InvalidateGPUMemory(u8 * p, size_t size)
    {
        constexpr size_t sys_core = Hardware::NUM_CPU_CORES - 1;
        const size_t core = std::min(system.GetCurrentHostThreadID(), sys_core);

        if (core == sys_core) [[unlikely]]
        {
            sys_core_guard.lock();
        }
        SCOPE_EXIT
        {
            if (core == sys_core) [[unlikely]]
            {
                sys_core_guard.unlock();
            }
        };

        system.GetVideo().InvalidateMemory(p, size);
    }

    uint64_t InvalidateNceGPUMemory(u8 * p, size_t size, size_t max_coalesce_size)
    {
        constexpr size_t sys_core = Hardware::NUM_CPU_CORES - 1;
        const size_t core = std::min(system.GetCurrentHostThreadID(), sys_core);

        if (core == sys_core) [[unlikely]]
        {
            sys_core_guard.lock();
        }
        SCOPE_EXIT
        {
            if (core == sys_core) [[unlikely]]
            {
                sys_core_guard.unlock();
            }
        };

        return system.GetVideo().InvalidateNceMemory(p, size, max_coalesce_size);
    }

    Core::System & system;
    const u64 rasterizer_readback_generation;
    Common::PageTable * current_page_table = nullptr;
    std::array<RasterizerDownloadArea, Hardware::NUM_CPU_CORES> rasterizer_read_areas{};
    std::array<GPUDirtyState, Hardware::NUM_CPU_CORES> rasterizer_write_areas{};
    std::span<Core::GPUDirtyMemoryManager> gpu_dirty_managers;
    std::mutex sys_core_guard;

#if !defined(__APPLE__)
    std::mutex read_cached_mutex;
    std::unordered_map<u64, u32> read_cached_counts;
    std::unordered_map<u64, u32> write_protected_counts;
    std::unique_ptr<u64[], FreeBits> read_cached_storage;
    std::atomic<u64 *> read_cached_bits{};
    uint64_t read_cached_page_count{};
    std::mutex harvest_mutex;
    std::vector<u64> harvested_pages;
#endif

#if defined(__APPLE__)
    // Native code execution on 16 KiB pages: a write to a host page holding guest pages the GPU
    // caches makes them writable instead of invalidating the GPU cache, which would make it
    // reload them even when the write went to another guest page. Their contents are compared
    // with a copy at the next GPU synchronization, when the writes of the JIT are reported too.
    static constexpr u64 NceHostPageSize = 0x4000;
    static constexpr size_t MaxPooledPageCopies = 256;
    using PageCopy = std::array<u8, YUZU_PAGESIZE>;
    struct NceWrittenEntry
    {
        std::unique_ptr<PageCopy> copy;
        uintptr_t phys_page{};
    };
    static uintptr_t PhysPageOf(const u8 * ptr) noexcept
    {
        return reinterpret_cast<uintptr_t>(ptr) & ~YUZU_PAGEMASK;
    }
    using PhysVAddrList = std::vector<std::pair<uintptr_t, u64>>;
    static PhysVAddrList::iterator LowerBoundPhys(PhysVAddrList & list, uintptr_t phys_page) noexcept
    {
        return std::lower_bound(
            list.begin(), list.end(), phys_page,
            [](const std::pair<uintptr_t, u64> & a, uintptr_t b) { return a.first < b; });
    }
    static void InsertPhysEntry(PhysVAddrList & list, uintptr_t phys_page, u64 page)
    {
        const std::pair<uintptr_t, u64> entry{phys_page, page};
        const auto pos = std::lower_bound(list.begin(), list.end(), entry);
        list.insert(pos, entry);
    }
    static void ErasePhysEntry(PhysVAddrList & list, uintptr_t phys_page, u64 page)
    {
        const std::pair<uintptr_t, u64> entry{phys_page, page};
        const auto it = std::lower_bound(list.begin(), list.end(), entry);
        if (it != list.end() && *it == entry)
        {
            list.erase(it);
        }
    }
    bool ActiveReadbackOverlapsVAddrLocked(u64 vaddr_begin, u64 vaddr_end) const
    {
        if (active_gpu_readbacks.empty()) [[likely]]
        {
            return false;
        }
        for (u64 page = vaddr_begin; page < vaddr_end; page += YUZU_PAGESIZE)
        {
            const u8 * const ptr = GetPointerImpl(page, []() {}, []() {});
            if (ptr == nullptr) continue;
            const uintptr_t phys = PhysPageOf(ptr);
            const uintptr_t phys_end = phys + YUZU_PAGESIZE;
            for (const auto & [rb_begin, rb_end] : active_gpu_readbacks)
            {
                if (rb_begin < phys_end && phys < rb_end)
                {
                    return true;
                }
            }
        }
        return false;
    }
    void WaitForActiveGpuReadbacksForVAddrLocked(std::unique_lock<std::mutex> & lk,
                                                 u64 vaddr_begin, u64 vaddr_end)
    {
        if (active_gpu_readbacks.empty()) [[likely]]
        {
            return;
        }
        if (!ActiveReadbackOverlapsVAddrLocked(vaddr_begin, vaddr_end)) [[likely]]
        {
            return;
        }
        ++active_readback_waiters;
        active_readback_cv.wait(lk, [&] {
            return !ActiveReadbackOverlapsVAddrLocked(vaddr_begin, vaddr_end);
        });
        --active_readback_waiters;
    }
    void RemoveActiveGpuReadbackLocked(uintptr_t phys_begin, uintptr_t phys_end)
    {
        for (auto it = active_gpu_readbacks.begin(); it != active_gpu_readbacks.end(); ++it)
        {
            if (it->first == phys_begin && it->second == phys_end)
            {
                active_gpu_readbacks.erase(it);
                break;
            }
        }
        if (active_readback_waiters > 0)
        {
            active_readback_cv.notify_all();
        }
    }
    std::unique_ptr<PageCopy> AcquirePageCopy()
    {
        if (!nce_page_pool.empty())
        {
            auto copy = std::move(nce_page_pool.back());
            nce_page_pool.pop_back();
            return copy;
        }
        return Common::make_unique_for_overwrite<PageCopy>();
    }
    void ReleasePageCopy(std::unique_ptr<PageCopy> copy)
    {
        if (copy && nce_page_pool.size() < MaxPooledPageCopies)
        {
            nce_page_pool.push_back(std::move(copy));
        }
    }
    std::mutex nce_write_mutex;
    std::condition_variable active_readback_cv;
    u32 active_readback_waiters{};
    std::vector<std::pair<uintptr_t, uintptr_t>> active_gpu_readbacks;
    std::atomic<bool> has_nce_written{false};
    std::atomic<bool> has_nce_flushing{false};
    std::atomic<bool> has_nce_open_read_pages{false};
    std::vector<std::unique_ptr<PageCopy>> nce_page_pool;
    std::map<u64, NceWrittenEntry> nce_written;
    std::map<u64, NceWrittenEntry> nce_flushing;
    PhysVAddrList nce_written_by_phys;
    PhysVAddrList nce_flushing_by_phys;
    struct NceReadCachedEntry
    {
        u32 count{};
        u32 generation{};
        bool direct_read{};
        u64 neighbor_defer_until_ns{};
    };
    std::map<u64, NceReadCachedEntry> nce_read_cached;
    std::set<u64> nce_open_read_pages;
    std::set<u64> nce_ever_direct_read_pages;
#endif

    std::optional<Common::HeapTracker> heap_tracker;
#ifdef __linux__
    Common::HeapTracker * buffer{};
#else
    Common::HostMemory * buffer{};
#endif
};

Memory::Memory(Core::System & system_) :
    system{system_}
{
    Reset();
}

Memory::~Memory() = default;

void Memory::Reset()
{
    impl = std::make_unique<Impl>(system);
}

void Memory::SetCurrentPageTable(Kernel::KProcess & process)
{
    impl->SetCurrentPageTable(process);
}

void Memory::MapMemoryRegion(Common::PageTable & page_table, Common::ProcessAddress base, u64 size, Common::PhysicalAddress target, Common::MemoryPermission perms, bool separate_heap)
{
    impl->MapMemoryRegion(page_table, base, size, target, perms, separate_heap);
}

void Memory::UnmapRegion(Common::PageTable & page_table, Common::ProcessAddress base, u64 size, bool separate_heap)
{
    impl->UnmapRegion(page_table, base, size, separate_heap);
}

void Memory::ProtectRegion(Common::PageTable & page_table, Common::ProcessAddress vaddr, u64 size, Common::MemoryPermission perms)
{
    impl->ProtectRegion(page_table, vaddr.GetValue(), size, perms);
}

bool Memory::IsValidVirtualAddress(const Common::ProcessAddress vaddr) const
{
    const auto & page_table = *impl->current_page_table;
    const size_t page = vaddr >> YUZU_PAGEBITS;
    if (page >= page_table.pointers.size())
    {
        return false;
    }
    const auto [pointer, type] = page_table.pointers[page].PointerType();
    return pointer != 0 || type == Common::PageType::RasterizerCachedMemory ||
           type == Common::PageType::DebugMemory;
}

bool Memory::IsValidVirtualAddressRange(uint64_t base, uint64_t size) const
{
    if (size == 0)
    {
        return true;
    }
    if (size - 1 > std::numeric_limits<uint64_t>::max() - base)
    {
        return false;
    }

    const Common::ProcessAddress last_page = Common::AlignDown(base + size - 1, YUZU_PAGESIZE);
    Common::ProcessAddress page = Common::AlignDown(base, YUZU_PAGESIZE);

    for (;; page += YUZU_PAGESIZE)
    {
        if (!IsValidVirtualAddress(page))
        {
            return false;
        }
        if (page == last_page)
        {
            return true;
        }
    }
}

u8 * Memory::TryGetPlainContiguousSpan(uint64_t base, uint64_t size)
{
    if (size == 0 || size - 1 > std::numeric_limits<uint64_t>::max() - base)
    {
        return nullptr;
    }
    const auto & page_table = *impl->current_page_table;
    const size_t first_page = base >> YUZU_PAGEBITS;
    const size_t last_page = (base + size - 1) >> YUZU_PAGEBITS;
    if (last_page >= page_table.pointers.size() || last_page >= page_table.blocks.size())
    {
        return nullptr;
    }
    if (page_table.blocks[first_page] != page_table.blocks[last_page])
    {
        return nullptr;
    }
    const auto [first_ptr, first_type] = page_table.pointers[first_page].PointerType();
    if (first_type != Common::PageType::Memory || first_ptr == 0)
    {
        return nullptr;
    }
    for (size_t page = first_page + 1; page <= last_page; ++page)
    {
        const auto [ptr, type] = page_table.pointers[page].PointerType();
        if (type != Common::PageType::Memory || ptr != first_ptr)
        {
            return nullptr;
        }
    }
    return reinterpret_cast<u8 *>(first_ptr + base);
}

u8 * Memory::GetPointer(Common::ProcessAddress vaddr)
{
    return impl->GetPointer(vaddr);
}

u8 * Memory::GetPointerSilent(uint64_t vaddr)
{
    return impl->GetPointerSilent(vaddr);
}

const u8 * Memory::GetPointer(Common::ProcessAddress vaddr) const
{
    return impl->GetPointer(vaddr);
}

uint8_t Memory::Read8(uint64_t addr)
{
    return impl->Read8(addr);
}

uint16_t Memory::Read16(uint64_t addr)
{
    return impl->Read16(addr);
}

uint32_t Memory::Read32(uint64_t addr)
{
    return impl->Read32(addr);
}

uint64_t Memory::Read64(uint64_t addr)
{
    return impl->Read64(addr);
}

void Memory::Write8(uint64_t addr, uint8_t data)
{
    impl->Write8(addr, data);
}

void Memory::Write16(uint64_t addr, uint16_t data)
{
    impl->Write16(addr, data);
}

void Memory::Write32(uint64_t addr, uint32_t data)
{
    impl->Write32(addr, data);
}

void Memory::Write64(uint64_t addr, uint64_t data)
{
    impl->Write64(addr, data);
}

bool Memory::WriteExclusive8(uint64_t addr, uint8_t data, uint8_t expected)
{
    return impl->WriteExclusive8(addr, data, expected);
}

bool Memory::WriteExclusive16(uint64_t addr, uint16_t data, uint16_t expected)
{
    return impl->WriteExclusive16(addr, data, expected);
}

bool Memory::WriteExclusive32(uint64_t addr, uint32_t data, uint32_t expected)
{
    return impl->WriteExclusive32(addr, data, expected);
}

bool Memory::WriteExclusive64(uint64_t addr, uint64_t data, uint64_t expected)
{
    return impl->WriteExclusive64(addr, data, expected);
}

bool Memory::WriteExclusive128(uint64_t addr, uint64_t dataHi, uint64_t dataLow, uint64_t expectedHi, uint64_t expectedLow)
{
    return impl->WriteExclusive128(addr, u128{dataLow, dataHi}, u128{expectedLow, expectedHi});
}

std::string Memory::ReadCString(Common::ProcessAddress vaddr, std::size_t max_length)
{
    return impl->ReadCString(vaddr, max_length);
}

bool Memory::ReadBlock(uint64_t src_addr, void * dest_buffer, uint64_t size)
{
    return impl->ReadBlock(src_addr, dest_buffer, size);
}

bool Memory::ReadBlockUnsafe(const Common::ProcessAddress src_addr, void * dest_buffer, const std::size_t size)
{
    return impl->ReadBlockUnsafe(src_addr, dest_buffer, size);
}

const u8 * Memory::GetSpan(const VAddr src_addr, const std::size_t size) const
{
    return impl->GetSpan(src_addr, size);
}

u8 * Memory::GetSpan(const VAddr src_addr, const std::size_t size)
{
    return impl->GetSpan(src_addr, size);
}

bool Memory::WriteBlock(const Common::ProcessAddress dest_addr, const void * src_buffer, const std::size_t size)
{
    return impl->WriteBlock(dest_addr, src_buffer, size);
}

bool Memory::WriteBlock(uint64_t dest_addr, const void * src_buffer, uint64_t size)
{
    return WriteBlock(Common::ProcessAddress{dest_addr}, src_buffer, (std::size_t)size);
}

bool Memory::WriteBlockUnsafe(const Common::ProcessAddress dest_addr, const void * src_buffer, const std::size_t size)
{
    return impl->WriteBlockUnsafe(dest_addr, src_buffer, size);
}

bool Memory::CopyBlock(Common::ProcessAddress dest_addr, Common::ProcessAddress src_addr,
                       const std::size_t size)
{
    return impl->CopyBlock(dest_addr, src_addr, size);
}

bool Memory::ZeroBlock(Common::ProcessAddress dest_addr, const std::size_t size)
{
    return impl->ZeroBlock(dest_addr, size);
}

void Memory::SetGPUDirtyManagers(std::span<Core::GPUDirtyMemoryManager> managers)
{
    impl->gpu_dirty_managers = managers;
}

Result Memory::InvalidateDataCache(Common::ProcessAddress dest_addr, const std::size_t size)
{
    return impl->InvalidateDataCache(dest_addr, size);
}

Result Memory::StoreDataCache(Common::ProcessAddress dest_addr, const std::size_t size)
{
    return impl->StoreDataCache(dest_addr, size);
}

Result Memory::FlushDataCache(Common::ProcessAddress dest_addr, const std::size_t size)
{
    return impl->FlushDataCache(dest_addr, size);
}

void Memory::RasterizerMarkRegionReadCached(uint64_t vaddr, uint64_t size, int32_t delta)
{
    impl->RasterizerMarkRegionReadCached(vaddr, size, delta);
}

void Memory::RasterizerMarkRegionWriteProtected(uint64_t vaddr, uint64_t size, int32_t delta)
{
#if !defined(__APPLE__)
    impl->RasterizerMarkRegionWriteProtected(vaddr, size, delta);
#endif
}

bool Memory::ProtectsReadCachedRegions()
{
    if (!osSettings.background_gpu_readbacks.load(std::memory_order_relaxed))
    {
        return false;
    }
#if defined(__APPLE__)
    return impl->buffer->UsesNce() && osSettings.use_reactive_flushing;
#elif defined(_M_X64) || defined(__x86_64__)
    return ProtectOnlyReadCachedPages() && impl->current_page_table &&
           impl->current_page_table->fastmem_arena;
#else
    return false;
#endif
}

bool Memory::ProtectsQueryResults()
{
#if defined(__APPLE__)
    return ProtectsReadCachedRegions();
#elif defined(_M_X64) || defined(__x86_64__)
    // Not tied to background readbacks: that setting only lets fences skip download waits.
    // Protected results are exact and cheap, and unprotected ones can be read stale.
    // Matches where RasterizerMarkRegionReadCached denies access
    return ProtectOnlyReadCachedPages() && impl->current_page_table &&
           impl->current_page_table->fastmem_arena;
#else
    // An NCE fault invalidates the page without waiting for the result
    return false;
#endif
}

void Memory::RasterizerMarkRegionCached(uint64_t vaddr, uint64_t size, bool cached)
{
    impl->RasterizerMarkRegionCached(vaddr, size, cached);
}

void Memory::MarkRegionDebug(Common::ProcessAddress vaddr, u64 size, bool debug)
{
    impl->MarkRegionDebug(vaddr.GetValue(), size, debug);
}

bool Memory::InvalidateNCE(uint64_t vaddr, uint64_t size)
{
    return InvalidateNCE(Common::ProcessAddress(vaddr), static_cast<size_t>(size));
}

bool Memory::InvalidateNCE(Common::ProcessAddress vaddr, size_t size)
{
    [[maybe_unused]] bool mapped = true;
    [[maybe_unused]] bool rasterizer = false;

    u8 * const ptr = impl->GetPointerImpl(
        vaddr.GetValue(),
        [&] {
            LOG_ERROR(HW_Memory, "Unmapped InvalidateNCE for {} bytes @ {:#x}", size,
                      vaddr.GetValue());
            mapped = false;
        },
        [&] { rasterizer = true; });
    if (rasterizer)
    {
#if defined(__linux__)
        // On Linux/Android NCE, sequential CPU writes to dynamic GPU-cached buffers fault
        // page-by-page via SIGSEGV. Escalate the unprotect window (16 KiB -> 64 KiB) across
        // contiguous RasterizerCachedMemory pages to cut SIGSEGV/mprotect churn. Only the buffer
        // cache drops these pages: other caches keep them protected.
        constinit thread_local Core::NceWriteStreams<> nce_streams{};
        const u64 page_addr = vaddr.GetValue();
        const auto fault = nce_streams.Begin(page_addr);
        // A read-cached page faults on reads too: that says nothing about the next writes
        const bool write_fault_only = !impl->IsReadCachedPage(page_addr >> YUZU_PAGEBITS);
        const u64 window_pages =
            size == YUZU_PAGESIZE && write_fault_only ? fault.window_pages : 1;
        size_t max_coalesce = size;
        for (u64 p = 1; p < window_pages; ++p) {
            const u64 next_vaddr = page_addr + p * YUZU_PAGESIZE;
            if (!AddressSpaceContains(*impl->current_page_table, next_vaddr, YUZU_PAGESIZE)) {
                break;
            }
            const u64 next_idx = next_vaddr >> YUZU_PAGEBITS;
            if (impl->current_page_table->pointers[next_idx].Type() != Common::PageType::RasterizerCachedMemory ||
                impl->IsReadCachedPage(next_idx)) {
                break;
            }
            bool next_mapped = true;
            u8* const next_ptr = impl->GetPointerImpl(next_vaddr, [&] { next_mapped = false; }, [] {});
            if (!next_mapped || next_ptr != ptr + p * YUZU_PAGESIZE) {
                break;
            }
            max_coalesce += YUZU_PAGESIZE;
        }
        const uint64_t invalidated = impl->InvalidateNceGPUMemory(ptr, size, max_coalesce);
        // A page another cache holds stays protected among the invalidated ones, and pages
        // already writable after them do not fault: expect the next fault at the first page
        // still protected, so neither case restarts the stream
        const u64 scan_end =
            page_addr + invalidated + Core::NceWriteStreams<>::MaxWritablePages * YUZU_PAGESIZE;
        const u64 next_page =
            Core::NextProtectedPage(page_addr + size, scan_end, YUZU_PAGESIZE, [&](u64 page) {
                return !AddressSpaceContains(*impl->current_page_table, page, YUZU_PAGESIZE) ||
                       impl->current_page_table->pointers[page >> YUZU_PAGEBITS].Type() ==
                           Common::PageType::RasterizerCachedMemory;
            });
        nce_streams.End(fault, next_page);
#else
        impl->InvalidateGPUMemory(ptr, size);
#endif
    }

#ifdef __linux__
    if (!rasterizer && mapped)
    {
        impl->buffer->DeferredMapSeparateHeap(vaddr.GetValue());
    }
#endif

#if defined(__APPLE__)
    // Retrying only helps when the GPU cache denied the access AND the 16 KiB host page
    // is actually openable for writes; unopenable host pages (RX neighbor / non-contiguous)
    // must fall through to single-instruction emulation instead of spinning forever.
    return rasterizer && impl->buffer->NceHostAccessAllowed(vaddr.GetValue(), true);
#else
    return mapped && ptr != nullptr;
#endif
}

void Memory::RasterizerSetNativeReadAccess(uint64_t vaddr, uint64_t size, bool readable)
{
#if defined(__APPLE__)
    impl->SetNativeReadAccess(vaddr, size, readable);
#endif
}

bool Memory::NceTrackReads(uint64_t vaddr)
{
#if defined(__APPLE__)
    static const bool profile = std::getenv("NXEMU_NCE_READBACK_PROFILE") != nullptr;
    const bool ready = impl->buffer->NceTrackedAccessAllowed(vaddr, false);
    // A native load can fault because a different 4 KiB page denies access to
    // their shared 16 KiB host page. Synchronize those neighbours too, as stores do:
    // otherwise every later load of this page is interpreted. Return false if the
    // load must still be interpreted through its backing.
    if (!ready || !impl->buffer->NceHostAccessAllowed(vaddr, false)) impl->DownloadNceHostPage(vaddr);
    bool host_ready = impl->buffer->NceHostAccessAllowed(vaddr, false);
    // Fast readback publishes GPU signals before the results of earlier queries are written,
    // and an emulated load reads guest memory directly. Wait for a result the load reads.
    if (!host_ready && impl->AwaitPendingResult(vaddr, sizeof(u64), true))
    {
        const auto started = std::chrono::steady_clock::now();
        while (impl->AwaitPendingResult(vaddr, sizeof(u64), false) &&
               std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100))
        {
            std::this_thread::yield();
        }
        impl->DownloadNceHostPage(vaddr);
        host_ready = impl->buffer->NceHostAccessAllowed(vaddr, false);
    }
    if (profile)
    {
        using Clock = std::chrono::steady_clock;
        static thread_local u64 reads{}, ready_reads{}, emulated{};
        static thread_local auto reported = Clock::now();
        ++reads;
        ready_reads += ready;
        emulated += !host_ready;
        const auto now = Clock::now();
        if (now - reported >= std::chrono::seconds(5))
        {
            std::fprintf(stderr, "[NCE-PAGE-FAULT] interval=%.3f reads=%llu target_ready=%llu host_blocked=%llu\n",
                         std::chrono::duration<double>(now - reported).count(),
                         (unsigned long long)reads, (unsigned long long)ready_reads,
                         (unsigned long long)emulated);
            reads = ready_reads = emulated = 0;
            reported = now;
        }
    }
    return host_ready;
#else
    return false;
#endif
}

bool Memory::NceTrackWrites(uint64_t vaddr)
{
#if defined(__APPLE__)
    return impl->NceTrackWrites(vaddr);
#else
    return false;
#endif
}

bool Memory::NcePrepareCanonicalWrite(uint64_t vaddr)
{
#if defined(__APPLE__)
    return impl->NceTrackWrites(vaddr, true);
#else
    return false;
#endif
}

bool Memory::WriteGpuReadback(uint64_t vaddr, uint8_t * dst, const uint8_t * src, uint64_t size,
                              void (*copy)(uint8_t *, const uint8_t *, uint64_t, void *), void * user)
{
#if defined(__APPLE__)
    return impl->WriteGpuReadback(vaddr, dst, src, size, copy, user);
#else
    return false;
#endif
}

void Memory::HarvestCpuWrites()
{
#if !defined(__APPLE__)
    impl->HarvestCpuWrites();
#endif
}

bool Memory::TryRunInPlaceGpuReadback(uint64_t vaddr, uint8_t * dst, uint64_t size,
                                      void (*fn)(uint8_t *, uint64_t, void *), void * user)
{
#if defined(__APPLE__)
    return impl->TryRunInPlaceGpuReadback(vaddr, dst, size, fn, user);
#else
    fn(dst, size, user);
    return true;
#endif
}

void Memory::FlushNceWrites(bool rearm_native_reads, bool defer_release)
{
#if defined(__APPLE__)
    impl->FlushNceWrites(rearm_native_reads, defer_release);
#endif
}

void Memory::CompleteNceWriteFlush()
{
#if defined(__APPLE__)
    impl->CompleteNceWriteFlush();
#endif
}

bool Memory::NceAccessAllowed(uint64_t vaddr, bool write)
{
    return system.DeviceMemory().buffer.NceAccessAllowed(vaddr, write);
}

bool Memory::InvalidateSeparateHeap(void * fault_address)
{
#ifdef __linux__
    return impl->buffer->DeferredMapSeparateHeap(static_cast<u8 *>(fault_address));
#else
    return false;
#endif
}

} // namespace Core::Memory
