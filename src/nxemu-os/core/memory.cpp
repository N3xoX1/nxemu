// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-FileCopyrightText: 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <limits>
#include <mutex>
#include <span>
#include <map>
#include <vector>

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/atomic_ops.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/hardware_properties.h"
#include "yuzu_common/heap_tracker.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/make_unique_for_overwrite.h"
#include "yuzu_common/page_table.h"
#include "yuzu_common/scope_exit.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include "nxemu-video/video_settings_identifiers.h"
#include "yuzu_common/swap.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/gpu_dirty_memory_manager.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "core/nce_written_ranges.h"
#include <nxemu-module-spec/video.h>

extern IModuleSettings* g_settings;

namespace Core::Memory {

namespace {

bool AddressSpaceContains(const Common::PageTable& table, const Common::ProcessAddress addr,
                          const std::size_t size) {
    const Common::ProcessAddress max_addr = 1ULL << table.GetAddressSpaceBits();
    return addr + size >= addr && addr + size <= max_addr;
}

} // namespace

// Implementation class used to keep the specifics of the memory subsystem hidden
// from outside classes. This also allows modification to the internals of the memory
// subsystem without needing to rebuild all files that make use of the memory interface.
struct Memory::Impl {
    explicit Impl(Core::System& system_) : system{system_} {}

    void SetCurrentPageTable(Kernel::KProcess& process) {
        current_page_table = &process.GetKPageTable().GetImpl();

        const bool fastmem = !g_settings->GetBool(NXCpuSetting::CpuDebugMode) ||
                             g_settings->GetBool(NXCpuSetting::CpuoptFastmem);
        if (std::addressof(process) == system.ApplicationProcess() && fastmem) {
            current_page_table->fastmem_arena = system.DeviceMemory().buffer.VirtualBasePointer();
        } else {
            current_page_table->fastmem_arena = nullptr;
        }

#ifdef __linux__
        heap_tracker.emplace(system.DeviceMemory().buffer);
        buffer = std::addressof(*heap_tracker);
#else
        buffer = std::addressof(system.DeviceMemory().buffer);
#endif
    }

    void MapMemoryRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                         Common::PhysicalAddress target, Common::MemoryPermission perms,
                         bool separate_heap) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", base.GetValue());
        ASSERT_MSG(target >= DramMemoryMap::Base, "Out of bounds target: {:016X}",
                   target.GetValue());
        MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, target,
                 Common::PageType::Memory);

        if (current_page_table->fastmem_arena) {
            buffer->Map(base.GetValue(), target.GetValue() - DramMemoryMap::Base, size, perms,
                        separate_heap);
        }
    }

    void UnmapRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size,
                     bool separate_heap) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((base & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", base.GetValue());
        MapPages(page_table, base / YUZU_PAGESIZE, size / YUZU_PAGESIZE, 0,
                 Common::PageType::Unmapped);

        if (current_page_table->fastmem_arena) {
            buffer->Unmap(base.GetValue(), size, separate_heap);
        }
    }

    void ProtectRegion(Common::PageTable& page_table, VAddr vaddr, u64 size,
                       Common::MemoryPermission perms) {
        ASSERT_MSG((size & YUZU_PAGEMASK) == 0, "non-page aligned size: {:016X}", size);
        ASSERT_MSG((vaddr & YUZU_PAGEMASK) == 0, "non-page aligned base: {:016X}", vaddr);

        if (!current_page_table->fastmem_arena) {
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
        for (u64 addr = vaddr; addr < vaddr + size; addr += YUZU_PAGESIZE) {
            const Common::PageType page_type{
                current_page_table->pointers[addr >> YUZU_PAGEBITS].Type()};
            switch (page_type) {
            case Common::PageType::RasterizerCachedMemory:
                if (protect_bytes > 0) {
                    buffer->Protect(protect_begin, protect_bytes, perms);
                    protect_bytes = 0;
                }
                break;
            default:
                if (protect_bytes == 0) {
                    protect_begin = addr;
                }
                protect_bytes += YUZU_PAGESIZE;
            }
        }

        if (protect_bytes > 0) {
            buffer->Protect(protect_begin, protect_bytes, perms);
        }
    }

    [[nodiscard]] u8* GetPointerFromRasterizerCachedMemory(u64 vaddr) const {
        const Common::PhysicalAddress paddr{
            current_page_table->backing_addr[vaddr >> YUZU_PAGEBITS]};

        if (!paddr) {
            return {};
        }

        return system.DeviceMemory().GetPointer<u8>(paddr + vaddr);
    }

    [[nodiscard]] u8* GetPointerFromDebugMemory(u64 vaddr) const {
        const Common::PhysicalAddress paddr{
            current_page_table->backing_addr[vaddr >> YUZU_PAGEBITS]};

        if (paddr == 0) {
            return {};
        }

        return system.DeviceMemory().GetPointer<u8>(paddr + vaddr);
    }

    u8 Read8(const Common::ProcessAddress addr) {
        return Read<u8>(addr);
    }

    u16 Read16(const Common::ProcessAddress addr) {
        if ((addr & 1) == 0) {
            return Read<u16_le>(addr);
        } else {
            const u32 a{Read<u8>(addr)};
            const u32 b{Read<u8>(addr + sizeof(u8))};
            return static_cast<u16>((b << 8) | a);
        }
    }

    u32 Read32(const Common::ProcessAddress addr) {
        if ((addr & 3) == 0) {
            return Read<u32_le>(addr);
        } else {
            const u32 a{Read16(addr)};
            const u32 b{Read16(addr + sizeof(u16))};
            return (b << 16) | a;
        }
    }

    u64 Read64(const Common::ProcessAddress addr) {
        if ((addr & 7) == 0) {
            return Read<u64_le>(addr);
        } else {
            const u32 a{Read32(addr)};
            const u32 b{Read32(addr + sizeof(u32))};
            return (static_cast<u64>(b) << 32) | a;
        }
    }

    void Write8(const Common::ProcessAddress addr, const u8 data) {
        Write<u8>(addr, data);
    }

    void Write16(const Common::ProcessAddress addr, const u16 data) {
        if ((addr & 1) == 0) {
            Write<u16_le>(addr, data);
        } else {
            Write<u8>(addr, static_cast<u8>(data));
            Write<u8>(addr + sizeof(u8), static_cast<u8>(data >> 8));
        }
    }

    void Write32(const Common::ProcessAddress addr, const u32 data) {
        if ((addr & 3) == 0) {
            Write<u32_le>(addr, data);
        } else {
            Write16(addr, static_cast<u16>(data));
            Write16(addr + sizeof(u16), static_cast<u16>(data >> 16));
        }
    }

    void Write64(const Common::ProcessAddress addr, const u64 data) {
        if ((addr & 7) == 0) {
            Write<u64_le>(addr, data);
        } else {
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
        const auto& page_table = *current_page_table;
        std::size_t remaining_size = size;
        std::size_t page_index = addr >> YUZU_PAGEBITS;
        std::size_t page_offset = addr & YUZU_PAGEMASK;
        bool user_accessible = true;

        if (!AddressSpaceContains(page_table, addr, size)) [[unlikely]] {
            on_unmapped(size, addr);
            return false;
        }

        while (remaining_size) {
            const std::size_t copy_amount =
                std::min(static_cast<std::size_t>(YUZU_PAGESIZE) - page_offset, remaining_size);
            const auto current_vaddr =
                static_cast<u64>((page_index << YUZU_PAGEBITS) + page_offset);

            const auto [pointer, type] = page_table.pointers[page_index].PointerType();
            switch (type) {
            case Common::PageType::Unmapped: {
                user_accessible = false;
                on_unmapped(copy_amount, current_vaddr);
                break;
            }
            case Common::PageType::Memory: {
                u8* mem_ptr =
                    reinterpret_cast<u8*>(pointer + page_offset + (page_index << YUZU_PAGEBITS));
                on_memory(copy_amount, mem_ptr);
                break;
            }
            case Common::PageType::DebugMemory: {
                u8* const mem_ptr{GetPointerFromDebugMemory(current_vaddr)};
                on_memory(copy_amount, mem_ptr);
                break;
            }
            case Common::PageType::RasterizerCachedMemory: {
                u8* const host_ptr{GetPointerFromRasterizerCachedMemory(current_vaddr)};
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
    bool ReadBlockImpl(const Common::ProcessAddress src_addr, void* dest_buffer,
                       const std::size_t size) {
        return WalkBlock(
            src_addr, size,
            [src_addr, size, &dest_buffer](const std::size_t copy_amount,
                                           const Common::ProcessAddress current_vaddr) {
                LOG_ERROR(HW_Memory,
                          "Unmapped ReadBlock @ 0x{:016X} (start address = 0x{:016X}, size = {})",
                          current_vaddr.GetValue(), src_addr.GetValue(), size);
                std::memset(dest_buffer, 0, copy_amount);
            },
            [&](const std::size_t copy_amount, const u8* const src_ptr) {
                std::memcpy(dest_buffer, src_ptr, copy_amount);
            },
            [&](const Common::ProcessAddress current_vaddr, const std::size_t copy_amount,
                const u8* const host_ptr) {
                if constexpr (!UNSAFE) {
                    HandleRasterizerDownload(current_vaddr.GetValue(), copy_amount);
                }
                std::memcpy(dest_buffer, host_ptr, copy_amount);
            },
            [&](const std::size_t copy_amount) {
                dest_buffer = static_cast<u8*>(dest_buffer) + copy_amount;
            });
    }

    bool ReadBlock(const Common::ProcessAddress src_addr, void* dest_buffer, const std::size_t size)
    {
        return ReadBlockImpl<false>(src_addr, dest_buffer, size);
    }

    bool ReadBlockUnsafe(const Common::ProcessAddress src_addr, void* dest_buffer, const std::size_t size)
    {
        return ReadBlockImpl<true>(src_addr, dest_buffer, size);
    }

    const u8 * GetSpan(const VAddr src_addr, const std::size_t size) const
    {
        if (current_page_table->blocks[src_addr >> YUZU_PAGEBITS] ==
            current_page_table->blocks[(src_addr + size) >> YUZU_PAGEBITS])
        {
            return GetPointerSilent(src_addr);
        }
        return nullptr;
    }

    u8 * GetSpan(const VAddr src_addr, const std::size_t size)
    {
        if (current_page_table->blocks[src_addr >> YUZU_PAGEBITS] ==
            current_page_table->blocks[(src_addr + size) >> YUZU_PAGEBITS])
        {
            return GetPointerSilent(src_addr);
        }
        return nullptr;
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
        if (vaddr == 0 || !AddressSpaceContains(*current_page_table, vaddr, size))
        {
            return;
        }
#if defined(__APPLE__)
        std::scoped_lock nce_lock{nce_write_mutex};
#endif

        if (current_page_table->fastmem_arena)
        {
            Common::MemoryPermission perm{};
            bool deny_reads = cached && g_settings->GetBool(NXVideoSetting::EnableReactiveFlushing);
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
            if (cached && buffer->UsesNce()) ProtectNceReadCached(vaddr, size);
            else buffer->ProtectAccess(vaddr, size, perm);
#else
            buffer->Protect(vaddr, size, perm);
#endif
        }

        // Iterate over a contiguous CPU address space, which corresponds to the specified GPU
        // address space, marking the region as un/cached. The region is marked un/cached at a
        // granularity of CPU pages, hence why we iterate on a CPU page basis (note: GPU page size
        // is different). This assumes the specified GPU address region is contiguous as well.

        const u64 num_pages = ((vaddr + size - 1) >> YUZU_PAGEBITS) - (vaddr >> YUZU_PAGEBITS) + 1;
        for (u64 i = 0; i < num_pages; ++i, vaddr += YUZU_PAGESIZE)
        {
            const Common::PageType page_type{
                current_page_table->pointers[vaddr >> YUZU_PAGEBITS].Type()};
            if (cached)
            {
                // Switch page type to cached if now cached
                switch (page_type)
                {
                case Common::PageType::Unmapped:
                    // It is not necessary for a process to have this region mapped into its address
                    // space, for example, a system module need not have a VRAM mapping.
                    break;
                case Common::PageType::DebugMemory:
                case Common::PageType::Memory:
                    current_page_table->pointers[vaddr >> YUZU_PAGEBITS].Store(
                        0, Common::PageType::RasterizerCachedMemory);
                    break;
                case Common::PageType::RasterizerCachedMemory:
                    // There can be more than one GPU region mapped per CPU region, so it's common
                    // that this area is already marked as cached.
                    break;
                default:
                    UNREACHABLE();
                }
            }
            else
            {
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
                    nce_written.erase(vaddr & ~YUZU_PAGEMASK);
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

    static void HandleRasterizerWriteCollect(uint64_t address, uint64_t size, void * user_data)
    {
        auto & context = *static_cast<RasterizerWriteCollectContext *>(user_data);
        context.impl->gpu_dirty_managers[context.core].Collect(address, size);
    }

    static void HandleRasterizerWriteCollectExact(uint64_t address, uint64_t size, void * user_data)
    {
        auto & context = *static_cast<RasterizerWriteCollectContext *>(user_data);
        context.impl->gpu_dirty_managers[context.core].CollectExact(address, size);
    }

#if defined(__APPLE__)
    // Caller holds nce_write_mutex. Apply each contiguous permission run once, and never
    // temporarily expose GPU-dirty pages while changing buffer write tracking.
    void ProtectNceReadCached(u64 vaddr, u64 size)
    {
        const u64 end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
        u64 cursor = Common::AlignDown(vaddr, YUZU_PAGESIZE);
        if (!g_settings->GetBool(NXVideoSetting::EnableReactiveFlushing))
        {
            buffer->ProtectAccess(cursor, end - cursor, Common::MemoryPermission::Read);
            return;
        }
        if (PreciseNceReadTracking())
        {
            u64 begin = cursor;
            auto permission = [&](u64 page) {
                return nce_read_cached.contains(page) && !nce_open_read_pages.contains(page)
                    ? Common::MemoryPermission{} : Common::MemoryPermission::Read;
            };
            auto current = permission(cursor);
            for (u64 page = cursor + YUZU_PAGESIZE; page < end; page += YUZU_PAGESIZE)
            {
                const auto next = permission(page);
                if (next != current)
                {
                    buffer->ProtectAccess(begin, page - begin, current);
                    begin = page;
                    current = next;
                }
            }
            if (begin < end) buffer->ProtectAccess(begin, end - begin, current);
            return;
        }
        auto it = nce_read_cached.lower_bound(cursor);
        while (it != nce_read_cached.end() && it->first < end)
        {
            const u64 begin = it->first;
            u64 run_end = begin;
            do {
                run_end += YUZU_PAGESIZE;
                ++it;
            } while (it != nce_read_cached.end() && it->first == run_end && it->first < end);
            if (cursor < begin)
                buffer->ProtectAccess(cursor, begin - cursor, Common::MemoryPermission::Read);
            buffer->ProtectAccess(begin, run_end - begin, Common::MemoryPermission{});
            cursor = run_end;
        }
        if (cursor < end)
            buffer->ProtectAccess(cursor, end - cursor, Common::MemoryPermission::Read);
    }

    void RasterizerMarkRegionReadCached(u64 vaddr, u64 size, s32 delta)
    {
        if (!buffer->UsesNce() || size == 0 || !AddressSpaceContains(*current_page_table, vaddr, size))
            return;
        std::scoped_lock lk{nce_write_mutex};
        const bool reactive = g_settings->GetBool(NXVideoSetting::EnableReactiveFlushing);
        u64 run_begin{}, run_end{};
        Common::MemoryPermission run_perm{};
        const auto flush = [&] {
            if (run_begin != run_end) buffer->ProtectAccess(run_begin, run_end - run_begin, run_perm);
            run_begin = run_end = 0;
        };
        const u64 end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
        for (u64 page = Common::AlignDown(vaddr, YUZU_PAGESIZE); page < end; page += YUZU_PAGESIZE)
        {
            auto it = nce_read_cached.find(page);
            if (delta > 0)
            {
                nce_open_read_pages.erase(page);
                if (it == nce_read_cached.end()) it = nce_read_cached.emplace(page, 0).first;
                it->second += static_cast<u32>(delta);
            }
            else if (it != nce_read_cached.end())
            {
                if (it->second <= static_cast<u32>(-delta))
                {
                    nce_read_cached.erase(it);
                    nce_open_read_pages.erase(page);
                    it = nce_read_cached.end();
                }
                else it->second -= static_cast<u32>(-delta);
            }
            if (current_page_table->pointers[page >> YUZU_PAGEBITS].Type() ==
                Common::PageType::RasterizerCachedMemory)
            {
                const bool protect = it != nce_read_cached.end() && reactive;
                const auto perm = protect ? Common::MemoryPermission{} : Common::MemoryPermission::Read;
                if (run_begin != run_end && (run_end != page || run_perm != perm)) flush();
                if (run_begin == run_end) { run_begin = page; run_perm = perm; }
                run_end = page + YUZU_PAGESIZE;
            }
            else flush();
        }
        flush();
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
        const u64 end = Common::AlignUp(vaddr + size, YUZU_PAGESIZE);
        for (u64 page = Common::AlignDown(vaddr, YUZU_PAGESIZE); page < end; page += YUZU_PAGESIZE)
        {
            const bool cached = current_page_table->pointers[page >> YUZU_PAGEBITS].Type() ==
                                Common::PageType::RasterizerCachedMemory;
            const bool tracked = cached && nce_read_cached.contains(page);
            if (readable)
            {
                if (tracked) nce_open_read_pages.insert(page);
                buffer->ProtectAccess(page, YUZU_PAGESIZE,
                    cached ? Common::MemoryPermission::Read : Common::MemoryPermission::ReadWrite);
            }
            else
            {
                nce_open_read_pages.erase(page);
                if (tracked) buffer->ProtectAccess(page, YUZU_PAGESIZE, Common::MemoryPermission{});
            }
        }
    }

    void RearmNceReads()
    {
        if (nce_open_read_pages.empty()) return;
        if (!g_settings->GetBool(NXVideoSetting::EnableReactiveFlushing))
        {
            nce_open_read_pages.clear();
            return;
        }
        u64 begin{}, end{};
        const auto flush = [&] {
            if (begin != end) buffer->ProtectAccess(begin, end - begin, Common::MemoryPermission{});
            begin = end = 0;
        };
        for (const u64 page : nce_open_read_pages)
        {
            if (!nce_read_cached.contains(page) ||
                current_page_table->pointers[page >> YUZU_PAGEBITS].Type() !=
                    Common::PageType::RasterizerCachedMemory)
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

    void DownloadNceHostPage(u64 vaddr)
    {
        std::array<u64, NceHostPageSize / YUZU_PAGESIZE> pages{};
        size_t count{};
        const u64 host_page = vaddr & ~(NceHostPageSize - 1);
        {
            std::scoped_lock lk{nce_write_mutex};
            if (!g_settings->GetBool(NXVideoSetting::EnableReactiveFlushing)) return;
            for (auto it = nce_read_cached.lower_bound(host_page);
                 it != nce_read_cached.end() && it->first < host_page + NceHostPageSize; ++it)
            {
                // A different 4 KiB neighbor may have trapped this 16 KiB host page.
                // Pages already synchronized remain readable until a real GPU write
                // revokes them on the producer; do not download them again here.
                if (!PreciseNceReadTracking() || !nce_open_read_pages.contains(it->first))
                    pages[count++] = it->first;
            }
        }
        // GPU readback may untrack textures and acquire nce_write_mutex. Do not hold it
        // while synchronizing. Read every protected neighbor before opening the host page.
        for (size_t i = 0; i < count;)
        {
            const u64 begin = pages[i++];
            u64 end = begin + YUZU_PAGESIZE;
            const auto pointer = reinterpret_cast<uintptr_t>(GetPointerFromRasterizerCachedMemory(begin));
            // Adjacent guest pages may map unrelated physical pages. Merge only when
            // their CPU backing is also contiguous.
            while (DemandNceReadback() && i < count && pages[i] == end && pointer != 0 &&
                   reinterpret_cast<uintptr_t>(GetPointerFromRasterizerCachedMemory(pages[i])) ==
                       pointer + end - begin)
            {
                end += YUZU_PAGESIZE;
                ++i;
            }
            HandleRasterizerDownload(begin, end - begin);
        }
        // Precise mode grants access and registers the observer on the GPU producer
        // before it resumes drawing. Reopening here could undo a subsequent GPU write.
        if (PreciseNceReadTracking()) return;
        std::scoped_lock lk{nce_write_mutex};
        for (size_t i = 0; i < count; ++i)
        {
            const bool cached = current_page_table->pointers[pages[i] >> YUZU_PAGEBITS].Type() ==
                                Common::PageType::RasterizerCachedMemory;
            if (DemandNceReadback() && cached && nce_read_cached.contains(pages[i]))
                nce_open_read_pages.insert(pages[i]);
            buffer->ProtectAccess(pages[i], YUZU_PAGESIZE, cached ? Common::MemoryPermission::Read
                                                               : Common::MemoryPermission::ReadWrite);
        }
    }

    bool NceTrackWrites(u64 vaddr, bool canonical = false)
    {
        std::unique_lock lk{nce_write_mutex};
        const u64 host_page = vaddr & ~(NceHostPageSize - 1);
        const u64 target_page = vaddr & ~YUZU_PAGEMASK;
        if (canonical) {
            if (!buffer->NceAccessAllowed(vaddr, true)) return false;
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
        if (g_settings->GetBool(NXVideoSetting::EnableReactiveFlushing))
        {
            const auto it = nce_read_cached.lower_bound(host_page);
            if (it != nce_read_cached.end() && it->first < host_page + NceHostPageSize)
            {
                lk.unlock();
                DownloadNceHostPage(vaddr);
                lk.lock();
                if (!(canonical ? buffer->NceTrackedAccessAllowed(vaddr, false)
                                : buffer->NceHostAccessAllowed(vaddr, false))) return false;
            }
        }
        // The GPU cache may have released the page since the fault.
        bool opened = buffer->NceHostAccessAllowed(vaddr, true);
        const u64 begin = canonical ? target_page : host_page;
        const u64 end = canonical ? target_page + YUZU_PAGESIZE : host_page + NceHostPageSize;
        for (u64 page = begin; page < end; page += YUZU_PAGESIZE)
        {
            if (current_page_table->pointers[page >> YUZU_PAGEBITS].Type() !=
                Common::PageType::RasterizerCachedMemory)
            {
                continue;
            }
            const u8 * const pointer = GetPointerFromRasterizerCachedMemory(page);
            if (pointer == nullptr)
            {
                continue;
            }
            auto [it, inserted] = nce_written.try_emplace(page);
            if (inserted)
            {
                // The snapshot is completely overwritten before the page becomes writable.
                it->second = Common::make_unique_for_overwrite<PageCopy>();
                std::memcpy(it->second->data(), pointer, YUZU_PAGESIZE);
            }
            // Readback may have write-protected a page already in nce_written. Restore
            // access for existing snapshots too, otherwise retrying the store loops forever.
            if (inserted || !buffer->NceHostAccessAllowed(vaddr, true))
                buffer->ProtectAccess(page, YUZU_PAGESIZE, Common::MemoryPermission::ReadWrite);
            opened = true;
        }
        // A fragmented or read-only guest mapping can still forbid a native store.
        // Such accesses must be interpreted rather than retried without progress.
        if (canonical) return buffer->NceTrackedAccessAllowed(vaddr, false) &&
                              buffer->NceTrackedAccessAllowed(vaddr, true);
        return opened && buffer->NceHostAccessAllowed(vaddr, true);
    }

    void FlushNceWrites(bool rearm_native_reads = true)
    {
        std::vector<std::pair<u64, u64>> written;
        {
            std::scoped_lock lk{nce_write_mutex};
            // Pages unmapped since are not cached anymore.
            std::erase_if(nce_written, [this](const auto & entry) {
                return current_page_table->pointers[entry.first >> YUZU_PAGEBITS].Type() !=
                       Common::PageType::RasterizerCachedMemory;
            });
            // Only pages opened by a native read are rearmed; untouched image pages
            // retain their existing protection without a per-image walk.
            if (rearm_native_reads && DemandNceReadback() && !PreciseNceReadTracking())
                RearmNceReads();
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
                if (rearm_native_reads) ProtectNceReadCached(begin, end - begin);
                else buffer->ProtectAccess(begin, end - begin, Common::MemoryPermission::Read);
            }
            // Report only the bytes the CPU changed. The rest of the page can hold GPU writes
            // that are not downloaded yet, invalidating it would upload stale guest memory.
            for (const auto & [page, copy] : nce_written)
            {
                const u8 * const pointer = GetPointerFromRasterizerCachedMemory(page);
                if (pointer == nullptr || std::memcmp(pointer, copy->data(), YUZU_PAGESIZE) == 0)
                {
                    continue;
                }
                ForEachNceWrittenRange(pointer, copy->data(), YUZU_PAGESIZE, [&](u64 offset, u64 size) {
                    written.emplace_back(page + offset, size);
                });
            }
            nce_written.clear();
        }
        for (const auto & [address, size] : written)
        {
            HandleRasterizerWrite(address, size, true);
        }
    }
#endif

    void HandleRasterizerDownload(VAddr v_address, size_t size)
    {
        const auto * p = GetPointerImpl(v_address, []() {}, []() {});
#if defined(__APPLE__)
        if (DemandNceReadback())
        {
            // Do not reuse a whole-image range across GPU submissions. It carries no
            // generation and could otherwise suppress a required native readback.
            RasterizerDownloadArea request{};
            request.native_read = true;
            system.GetVideo().HandleRasterizerDownload(p, size, request);
            return;
        }
#endif
        const size_t core = system.GetCurrentHostThreadID();
        RasterizerDownloadArea & area = rasterizer_read_areas[core];
        area = system.GetVideo().HandleRasterizerDownload(p, size, area);
    }

    void HandleRasterizerWrite(VAddr v_address, size_t size, bool exact = false)
    {
        const auto * p = GetPointerImpl(v_address, []() {}, []() {});
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
        system.GetVideo().HandleRasterizerWrite(p, size, &rasterizer_write_areas[core].last_address,
            exact ? HandleRasterizerWriteCollectExact : HandleRasterizerWriteCollect, &context);
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

    Core::System& system;
    Common::PageTable* current_page_table = nullptr;
    std::array<RasterizerDownloadArea, Hardware::NUM_CPU_CORES> rasterizer_read_areas{};
    std::array<GPUDirtyState, Hardware::NUM_CPU_CORES> rasterizer_write_areas{};
    std::span<Core::GPUDirtyMemoryManager> gpu_dirty_managers;
    std::mutex sys_core_guard;

#if defined(__APPLE__)
    // Native code execution on 16 KiB pages: a write to a host page holding guest pages the GPU
    // caches makes them writable instead of invalidating the GPU cache, which would make it
    // reload them even when the write went to another guest page. Their contents are compared
    // with a copy at the next GPU synchronization, when the writes of the JIT are reported too.
    static constexpr u64 NceHostPageSize = 0x4000;
    using PageCopy = std::array<u8, YUZU_PAGESIZE>;
    std::mutex nce_write_mutex;
    std::map<u64, std::unique_ptr<PageCopy>> nce_written;
    std::map<u64, u32> nce_read_cached;
    std::set<u64> nce_open_read_pages;
#endif

    std::optional<Common::HeapTracker> heap_tracker;
#ifdef __linux__
    Common::HeapTracker* buffer{};
#else
    Common::HostMemory* buffer{};
#endif
};

Memory::Memory(Core::System& system_) :
    system{system_}
{
    Reset();
}

Memory::~Memory() = default;

void Memory::Reset()
{
    impl = std::make_unique<Impl>(system);
}

void Memory::SetCurrentPageTable(Kernel::KProcess& process)
{
    impl->SetCurrentPageTable(process);
}

void Memory::MapMemoryRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size, Common::PhysicalAddress target, Common::MemoryPermission perms, bool separate_heap)
{
    impl->MapMemoryRegion(page_table, base, size, target, perms, separate_heap);
}

void Memory::UnmapRegion(Common::PageTable& page_table, Common::ProcessAddress base, u64 size, bool separate_heap) {
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

u8* Memory::GetPointer(Common::ProcessAddress vaddr)
{
    return impl->GetPointer(vaddr);
}

u8* Memory::GetPointerSilent(uint64_t vaddr)
{
    return impl->GetPointerSilent(vaddr);
}

const u8* Memory::GetPointer(Common::ProcessAddress vaddr) const
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

bool Memory::ReadBlockUnsafe(const Common::ProcessAddress src_addr, void* dest_buffer, const std::size_t size)
{
    return impl->ReadBlockUnsafe(src_addr, dest_buffer, size);
}

const u8* Memory::GetSpan(const VAddr src_addr, const std::size_t size) const
{
    return impl->GetSpan(src_addr, size);
}

u8* Memory::GetSpan(const VAddr src_addr, const std::size_t size)
{
    return impl->GetSpan(src_addr, size);
}

bool Memory::WriteBlock(const Common::ProcessAddress dest_addr, const void* src_buffer, const std::size_t size)
{
    return impl->WriteBlock(dest_addr, src_buffer, size);
}

bool Memory::WriteBlock(uint64_t dest_addr, const void * src_buffer, uint64_t size)
{
    return WriteBlock(Common::ProcessAddress{dest_addr}, src_buffer, (std::size_t)size);
}

bool Memory::WriteBlockUnsafe(const Common::ProcessAddress dest_addr, const void* src_buffer, const std::size_t size) 
{
    return impl->WriteBlockUnsafe(dest_addr, src_buffer, size);
}

bool Memory::CopyBlock(Common::ProcessAddress dest_addr, Common::ProcessAddress src_addr,
                       const std::size_t size) {
    return impl->CopyBlock(dest_addr, src_addr, size);
}

bool Memory::ZeroBlock(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->ZeroBlock(dest_addr, size);
}

void Memory::SetGPUDirtyManagers(std::span<Core::GPUDirtyMemoryManager> managers) {
    impl->gpu_dirty_managers = managers;
}

Result Memory::InvalidateDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->InvalidateDataCache(dest_addr, size);
}

Result Memory::StoreDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->StoreDataCache(dest_addr, size);
}

Result Memory::FlushDataCache(Common::ProcessAddress dest_addr, const std::size_t size) {
    return impl->FlushDataCache(dest_addr, size);
}

void Memory::RasterizerMarkRegionReadCached(uint64_t vaddr, uint64_t size, int32_t delta) {
#if defined(__APPLE__)
    impl->RasterizerMarkRegionReadCached(vaddr, size, delta);
#endif
}

void Memory::RasterizerMarkRegionCached(uint64_t vaddr, uint64_t size, bool cached) {
    impl->RasterizerMarkRegionCached(vaddr, size, cached);
}

void Memory::MarkRegionDebug(Common::ProcessAddress vaddr, u64 size, bool debug) {
    impl->MarkRegionDebug(vaddr.GetValue(), size, debug);
}

bool Memory::InvalidateNCE(uint64_t vaddr, uint64_t size) {
    return InvalidateNCE(Common::ProcessAddress(vaddr), static_cast<size_t>(size));
}

bool Memory::InvalidateNCE(Common::ProcessAddress vaddr, size_t size) {
    [[maybe_unused]] bool mapped = true;
    [[maybe_unused]] bool rasterizer = false;

    u8* const ptr = impl->GetPointerImpl(
        vaddr.GetValue(),
        [&] {
            LOG_ERROR(HW_Memory, "Unmapped InvalidateNCE for {} bytes @ {:#x}", size,
                      vaddr.GetValue());
            mapped = false;
        },
        [&] { rasterizer = true; });
    if (rasterizer) {
        impl->InvalidateGPUMemory(ptr, size);
    }

#ifdef __linux__
    if (!rasterizer && mapped) {
        impl->buffer->DeferredMapSeparateHeap(vaddr.GetValue());
    }
#endif

#if defined(__APPLE__)
    // Retrying only helps when the GPU cache denied the access; other faults on mapped pages
    // come from host pages shared with other guest pages, and are emulated.
    return rasterizer;
#else
    return mapped && ptr != nullptr;
#endif
}

void Memory::RasterizerSetNativeReadAccess(uint64_t vaddr, uint64_t size, bool readable) {
#if defined(__APPLE__)
    impl->SetNativeReadAccess(vaddr, size, readable);
#endif
}

bool Memory::NceTrackReads(uint64_t vaddr) {
#if defined(__APPLE__)
    static const bool profile = std::getenv("NXEMU_NCE_READBACK_PROFILE") != nullptr;
    const bool ready = impl->buffer->NceTrackedAccessAllowed(vaddr, false);
    // A native load can fault because a different 4 KiB page denies access to
    // their shared 16 KiB host page. Synchronize those neighbours too, as stores do:
    // otherwise every later load of this page is interpreted. Return false if the
    // load must still be interpreted through its backing.
    if (!ready || !impl->buffer->NceHostAccessAllowed(vaddr, false)) impl->DownloadNceHostPage(vaddr);
    const bool host_ready = impl->buffer->NceHostAccessAllowed(vaddr, false);
    if (profile) {
        using Clock = std::chrono::steady_clock;
        static thread_local u64 reads{}, ready_reads{}, emulated{};
        static thread_local auto reported = Clock::now();
        ++reads;
        ready_reads += ready;
        emulated += !host_ready;
        const auto now = Clock::now();
        if (now - reported >= std::chrono::seconds(5)) {
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

bool Memory::NceTrackWrites(uint64_t vaddr) {
#if defined(__APPLE__)
    return impl->NceTrackWrites(vaddr);
#else
    return false;
#endif
}

bool Memory::NcePrepareCanonicalWrite(uint64_t vaddr) {
#if defined(__APPLE__)
    return impl->NceTrackWrites(vaddr, true);
#else
    return false;
#endif
}

void Memory::FlushNceWrites(bool rearm_native_reads) {
#if defined(__APPLE__)
    impl->FlushNceWrites(rearm_native_reads);
#endif
}

bool Memory::NceAccessAllowed(uint64_t vaddr, bool write) {
    return system.DeviceMemory().buffer.NceAccessAllowed(vaddr, write);
}

bool Memory::InvalidateSeparateHeap(void* fault_address) {
#ifdef __linux__
    return impl->buffer->DeferredMapSeparateHeap(static_cast<u8*>(fault_address));
#else
    return false;
#endif
}

} // namespace Core::Memory
