// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <unordered_set>
#include <cstring>
#include <utility>

#include "yuzu_common/alignment.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/yuzu_assert.h"
#include "core/core.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/devices/ioctl_serialization.h"
#include "core/hle/service/nvdrv/devices/nvhost_as_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_gpu.h"
#include "core/hle/service/nvdrv/nvdrv.h"

namespace Service::Nvidia::Devices {

nvhost_as_gpu::nvhost_as_gpu(Core::System& system_, Module& module_, NvCore::Container& core)
    : nvdevice{system_}, module{module_}, container{core}, nvmap{core.GetNvMapFile()}, vm{},
      gmmu{} 
{
}

nvhost_as_gpu::~nvhost_as_gpu() = default;

NvResult nvhost_as_gpu::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output)
{
    switch (command.group) {
    case 'A':
        switch (command.cmd) {
        case 0x1:
            return WrapFixed(this, &nvhost_as_gpu::BindChannel, input, output);
        case 0x2:
            return WrapFixed(this, &nvhost_as_gpu::AllocateSpace, input, output);
        case 0x3:
            return WrapFixed(this, &nvhost_as_gpu::FreeSpace, input, output);
        case 0x5:
            return WrapFixed(this, &nvhost_as_gpu::UnmapBuffer, input, output);
        case 0x6:
            return WrapFixed(this, &nvhost_as_gpu::MapBufferEx, input, output);
        case 0x8:
            return WrapFixed(this, &nvhost_as_gpu::GetVARegions1, input, output);
        case 0x9:
            return WrapFixed(this, &nvhost_as_gpu::AllocAsEx, input, output);
        case 0x14:
            return WrapVariable(this, &nvhost_as_gpu::Remap, input, output);
        default:
            break;
        }
        break;
    default:
        break;
    }

    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_as_gpu::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<const u8> inline_input, std::span<u8> output)
{
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_as_gpu::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input, std::span<u8> output, std::span<u8> inline_output)
{
    switch (command.group) {
    case 'A':
        switch (command.cmd) {
        case 0x8:
            return WrapFixedInlOut(this, &nvhost_as_gpu::GetVARegions3, input, output,
                                   inline_output);
        default:
            break;
        }
        break;
    default:
        break;
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

void nvhost_as_gpu::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {}
void nvhost_as_gpu::OnClose(DeviceFD fd) {
    std::scoped_lock lock(mutex);
    // Closing a descriptor does not retire its GMMU or drain work on a bound channel.
    // Retain only distinct pin tokens, not the AS object or its interval trees.
    std::unordered_set<SparsePin*> retained;
    for (auto& [offset, allocation] : allocation_map) {
        if (allocation.sparse_mappings.empty()) {
            continue;
        }
        if (!has_bound_channels) {
            system.GetVideo().Unmap(gmmu, offset, allocation.size);
        } else {
            for (const auto& [start, mapping] : allocation.sparse_mappings) {
                if (retained.insert(mapping.pin.get()).second) {
                    module.RetainNvMapPinUntilShutdown(mapping.pin->handle, mapping.pin);
                }
            }
        }
        allocation.sparse_mappings.clear();
    }
}

NvResult nvhost_as_gpu::AllocAsEx(IoctlAllocAsEx& params) {
    LOG_DEBUG(Service_NVDRV, "called, big_page_size=0x{:X}", params.big_page_size);

    std::scoped_lock lock(mutex);

    if (vm.initialised) {
        ASSERT_MSG(false, "Cannot initialise an address space twice!");
        return NvResult::InvalidState;
    }

    if (params.big_page_size) {
        if (!std::has_single_bit(params.big_page_size)) {
            LOG_ERROR(Service_NVDRV, "Non power-of-2 big page size: 0x{:X}!", params.big_page_size);
            return NvResult::BadValue;
        }

        if ((params.big_page_size & VM::SUPPORTED_BIG_PAGE_SIZES) == 0) {
            LOG_ERROR(Service_NVDRV, "Unsupported big page size: 0x{:X}!", params.big_page_size);
            return NvResult::BadValue;
        }

        vm.big_page_size = params.big_page_size;
        vm.big_page_size_bits = static_cast<u32>(std::countr_zero(params.big_page_size));

        vm.va_range_start = params.big_page_size << VM::VA_START_SHIFT;
    }

    // If this is unspecified then default values should be used
    if (params.va_range_start) {
        vm.va_range_start = params.va_range_start;
        vm.va_range_split = params.va_range_split;
        vm.va_range_end = params.va_range_end;
    }

    const u64 max_big_page_bits = Common::Log2Ceil64(vm.va_range_end);

    const auto start_pages{static_cast<u32>(vm.va_range_start >> VM::PAGE_SIZE_BITS)};
    const auto end_pages{static_cast<u32>(vm.va_range_split >> VM::PAGE_SIZE_BITS)};
    vm.small_page_allocator = std::make_shared<VM::Allocator>(start_pages, end_pages);

    const auto start_big_pages{static_cast<u32>(vm.va_range_split >> vm.big_page_size_bits)};
    const auto end_big_pages{static_cast<u32>(vm.va_range_end >> vm.big_page_size_bits)};
    vm.big_page_allocator = std::make_unique<VM::Allocator>(start_big_pages, end_big_pages);

    IVideo & video = system.GetVideo();
    gmmu = video.AllocAsEx(max_big_page_bits, vm.va_range_split, vm.big_page_size_bits, VM::PAGE_SIZE_BITS);
    vm.initialised = true;


    return NvResult::Success;
}

NvResult nvhost_as_gpu::AllocateSpace(IoctlAllocSpace& params)
{
    LOG_DEBUG(Service_NVDRV, "called, pages={:X}, page_size={:X}, flags={:X}", params.pages, params.page_size, params.flags);

    std::scoped_lock lock(mutex);

    if (!vm.initialised)
    {
        return NvResult::BadValue;
    }

    if (params.pages == 0 ||
        (params.page_size != VM::YUZU_PAGESIZE && params.page_size != vm.big_page_size))
    {
        return NvResult::BadValue;
    }

    if (params.page_size != vm.big_page_size && ((params.flags & MappingFlags::Sparse) != MappingFlags::None)) 
    {
        UNIMPLEMENTED_MSG("Sparse small pages are not implemented!");
        return NvResult::NotImplemented;
    }

    const u32 page_size_bits{params.page_size == VM::YUZU_PAGESIZE ? VM::PAGE_SIZE_BITS : vm.big_page_size_bits};
    auto & allocator{params.page_size == VM::YUZU_PAGESIZE ? *vm.small_page_allocator : *vm.big_page_allocator};

    if ((params.flags & MappingFlags::Fixed) != MappingFlags::None)
    {
        const u64 first_page = params.offset >> page_size_bits;
        if (!Common::IsAligned(params.offset, params.page_size) ||
            first_page < allocator.GetVAStart() || first_page >= allocator.GetVALimit() ||
            params.pages > allocator.GetVALimit() - first_page)
        {
            return NvResult::BadValue;
        }

        const u64 size = static_cast<u64>(params.pages) * params.page_size;
        const auto next_alloc = allocation_map.lower_bound(params.offset);
        const auto next_mapping = mapping_map.lower_bound(params.offset);
        if ((next_alloc != allocation_map.end() && next_alloc->first - params.offset < size) ||
            (next_alloc != allocation_map.begin() &&
             params.offset - std::prev(next_alloc)->first < std::prev(next_alloc)->second.size) ||
            (next_mapping != mapping_map.end() && next_mapping->first - params.offset < size) ||
            (next_mapping != mapping_map.begin() &&
             params.offset - std::prev(next_mapping)->first < std::prev(next_mapping)->second->size))
        {
            return NvResult::BadValue;
        }
        allocator.AllocateFixed(static_cast<u32>(first_page), params.pages);
    }
    else 
    {
        params.offset = static_cast<u64>(allocator.Allocate(params.pages)) << page_size_bits;
        if (!params.offset)
        {
            ASSERT_MSG(false, "Failed to allocate free space in the GPU AS!");
            return NvResult::InsufficientMemory;
        }
    }

    u64 size{static_cast<u64>(params.pages) * params.page_size};

    if ((params.flags & MappingFlags::Sparse) != MappingFlags::None)
    {
        IVideo & video = system.GetVideo();
        video.MapSparse(gmmu, params.offset, size, true);
    }

    allocation_map[params.offset] = {
        .size = size,
        .mappings{},
        .sparse_mappings{},
        .page_size = params.page_size,
        .sparse = (params.flags & MappingFlags::Sparse) != MappingFlags::None,
        .big_pages = params.page_size != VM::YUZU_PAGESIZE,
    };
    return NvResult::Success;
}

bool nvhost_as_gpu::FreeMappingLocked(u64 offset)
{
    const auto it = mapping_map.find(offset);
    if (it == mapping_map.end() || !it->second)
    {
        return false;
    }

    const auto mapping = it->second;
    u64 unmap_size = mapping->size;
    if (!mapping->fixed)
    {
        auto& allocator{mapping->big_page ? *vm.big_page_allocator : *vm.small_page_allocator};
        const u32 page_size_bits{mapping->big_page ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS};
        const u32 page_size{mapping->big_page ? vm.big_page_size : VM::YUZU_PAGESIZE};
        unmap_size = Common::AlignUp(mapping->size, page_size);
        allocator.Free(static_cast<u32>(mapping->offset >> page_size_bits),
                       static_cast<u32>(unmap_size >> page_size_bits));
    }

    IVideo& video = system.GetVideo();
    if (mapping->sparse_alloc)
    {
        video.MapSparse(gmmu, mapping->offset, mapping->size, mapping->big_page);
        auto allocation = allocation_map.upper_bound(mapping->offset);
        if (allocation != allocation_map.begin()) {
            --allocation;
            if (mapping->offset >= allocation->first &&
                mapping->offset - allocation->first < allocation->second.size) {
                ReplaceSparseOwnershipLocked(allocation->second, mapping->offset, mapping->size,
                                             nullptr);
            }
        }
    }
    else
    {
        video.Unmap(gmmu, mapping->offset, unmap_size);
    }

    nvmap.UnpinHandle(mapping->handle);

    if (mapping->fixed)
    {
        auto allocation = allocation_map.upper_bound(mapping->offset);
        if (allocation != allocation_map.begin())
        {
            --allocation;
            if (mapping->offset >= allocation->first &&
                mapping->offset - allocation->first < allocation->second.size)
            {
                allocation->second.mappings.erase(mapping->allocation_entry);
            }
        }
    }

    mapping_map.erase(it);
    return true;
}

NvResult nvhost_as_gpu::FreeSpace(IoctlFreeSpace& params)
{
    LOG_DEBUG(Service_NVDRV, "called, offset={:X}, pages={:X}, page_size={:X}", params.offset, params.pages, params.page_size);

    std::scoped_lock lock(mutex);

    if (!vm.initialised)
    {
        return NvResult::BadValue;
    }

    const auto it = allocation_map.find(params.offset);
    if (it == allocation_map.end())
    {
        LOG_WARNING(Service_NVDRV,
                    "Cannot free unknown GPU allocation offset=0x{:X} pages={} page_size=0x{:X}",
                    params.offset, params.pages, params.page_size);
        return NvResult::BadValue;
    }

    auto& allocation = it->second;
    if (allocation.page_size != params.page_size ||
        allocation.size != (static_cast<u64>(params.pages) * params.page_size))
    {
        LOG_WARNING(Service_NVDRV,
                    "GPU allocation size/page mismatch offset=0x{:X} request_size=0x{:X} stored_size=0x{:X} request_page=0x{:X} stored_page=0x{:X}",
                    params.offset, static_cast<u64>(params.pages) * params.page_size,
                    allocation.size, params.page_size, allocation.page_size);
        return NvResult::BadValue;
    }

    while (!allocation.mappings.empty())
    {
        const auto mapping = allocation.mappings.front();
        if (!mapping || !FreeMappingLocked(mapping->offset))
        {
            return NvResult::BadValue;
        }
    }

    if (allocation.sparse)
    {
        system.GetVideo().Unmap(gmmu, params.offset, allocation.size);
        // Dropping the last reference to each SparsePin balances the PinHandle call that
        // established the corresponding sparse REMAP ownership. Split intervals share the
        // same token, so a partially overwritten REMAP is unpinned only after its last
        // surviving interval disappears.
        allocation.sparse_mappings.clear();
    }

    auto& allocator{params.page_size == VM::YUZU_PAGESIZE ? *vm.small_page_allocator
                                                           : *vm.big_page_allocator};
    const u32 page_size_bits{params.page_size == VM::YUZU_PAGESIZE
                                 ? VM::PAGE_SIZE_BITS
                                 : vm.big_page_size_bits};

    allocator.Free(static_cast<u32>(params.offset >> page_size_bits),
                   static_cast<u32>(allocation.size >> page_size_bits));
    allocation_map.erase(it);
    return NvResult::Success;
}

std::size_t nvhost_as_gpu::ReplaceSparseOwnershipLocked(
    Allocation& allocation, u64 start, u64 size, std::shared_ptr<SparsePin> new_pin) {
    if (size == 0) {
        return 0;
    }

    const u64 end = start + size;
    auto& sparse_mappings = allocation.sparse_mappings;
    auto it = sparse_mappings.lower_bound(start);
    if (it != sparse_mappings.begin()) {
        auto previous = std::prev(it);
        if (previous->second.size > start - previous->first) {
            it = previous;
        }
    }

    std::vector<std::pair<u64, SparseMapping>> survivors;
    std::size_t replaced_segments{};
    while (it != sparse_mappings.end() && it->first < end) {
        const u64 old_start = it->first;
        const SparseMapping old_mapping = it->second;
        if (old_start <= start && old_mapping.size <= start - old_start) {
            ++it;
            continue;
        }
        const u64 old_end = old_start + old_mapping.size;

        auto erase_it = it++;
        sparse_mappings.erase(erase_it);
        ++replaced_segments;

        if (old_start < start) {
            survivors.emplace_back(
                old_start, SparseMapping{.size = start - old_start, .pin = old_mapping.pin});
        }
        if (old_end > end) {
            survivors.emplace_back(
                end, SparseMapping{.size = old_end - end, .pin = old_mapping.pin});
        }
    }

    for (auto& [survivor_start, survivor] : survivors) {
        sparse_mappings.emplace(survivor_start, std::move(survivor));
    }
    if (new_pin) {
        sparse_mappings.emplace(start, SparseMapping{.size = size, .pin = std::move(new_pin)});
    }
    return replaced_segments;
}

NvResult nvhost_as_gpu::Remap(std::span<IoctlRemapEntry> entries) 
{
    LOG_DEBUG(Service_NVDRV, "called, num_entries=0x{:X}", entries.size());

    std::scoped_lock lock(mutex);

    if (!vm.initialised) 
    {
        return NvResult::BadValue;
    }

    for (const auto& entry : entries)
    {
        const GPUVAddr virtual_address{static_cast<u64>(entry.as_offset_big_pages)
                                       << vm.big_page_size_bits};
        const u64 size{static_cast<u64>(entry.big_pages) << vm.big_page_size_bits};

        auto alloc{allocation_map.upper_bound(virtual_address)};
        if (alloc == allocation_map.begin())
        {
            LOG_WARNING(Service_NVDRV, "Cannot remap into an unallocated region!");
            return NvResult::BadValue;
        }
        --alloc;

        const u64 allocation_offset = virtual_address - alloc->first;
        if (allocation_offset > alloc->second.size ||
            size > alloc->second.size - allocation_offset)
        {
            LOG_WARNING(Service_NVDRV,
                        "Sparse remap outside allocation va=0x{:X} size=0x{:X} allocation=[0x{:X},0x{:X})",
                        virtual_address, size, alloc->first, alloc->first + alloc->second.size);
            return NvResult::BadValue;
        }

        if (!alloc->second.sparse)
        {
            LOG_WARNING(Service_NVDRV, "Cannot remap a non-sparse mapping!");
            return NvResult::BadValue;
        }

        const bool use_big_pages = alloc->second.big_pages;
        IVideo& video = system.GetVideo();
        std::shared_ptr<SparsePin> new_pin;
        u64 handle_offset{};
        if (!entry.handle)
        {
            video.MapSparse(gmmu, virtual_address, size, use_big_pages);
        }
        else
        {
            auto handle{nvmap.GetHandle(entry.handle)};
            if (!handle)
            {
                LOG_WARNING(Service_NVDRV,
                            "Sparse remap uses invalid nvmap handle=0x{:X} va=0x{:X}",
                            entry.handle, virtual_address);
                return NvResult::BadValue;
            }

            handle_offset = static_cast<u64>(entry.handle_offset_big_pages)
                            << vm.big_page_size_bits;
            if (!handle->allocated || handle_offset > handle->aligned_size ||
                size > handle->aligned_size - handle_offset)
            {
                LOG_WARNING(Service_NVDRV,
                            "Sparse remap exceeds nvmap backing handle=0x{:X} handle_offset=0x{:X} size=0x{:X} logical_size=0x{:X} aligned_size=0x{:X}",
                            entry.handle, handle_offset, size, handle->size, handle->aligned_size);
                return NvResult::BadValue;
            }

            const DAddr base = nvmap.PinHandle(entry.handle, false);
            new_pin = std::make_shared<SparsePin>(nvmap, entry.handle);
            const DAddr device_address{static_cast<DAddr>(base + handle_offset)};
            video.MapBufferEx(gmmu, virtual_address, device_address, size, entry.kind,
                              use_big_pages);
        }

        ReplaceSparseOwnershipLocked(alloc->second, virtual_address, size, std::move(new_pin));
    }
    return NvResult::Success;
}

NvResult nvhost_as_gpu::MapBufferEx(IoctlMapBufferEx & params)
{
    LOG_DEBUG(Service_NVDRV, "called, flags={:X}, nvmap_handle={:X}, buffer_offset={}, mapping_size={}, offset={}", params.flags, params.handle, params.buffer_offset, params.mapping_size, params.offset);

    std::scoped_lock lock(mutex);

    if (!vm.initialised)
    {
        return NvResult::BadValue;
    }

    // Remaps a subregion of an existing mapping to a different PA
    if ((params.flags & MappingFlags::Remap) != MappingFlags::None) 
    {
        try 
        {
            auto mapping{mapping_map.at(params.offset)};

            if (params.buffer_offset > mapping->size ||
                params.mapping_size > mapping->size - params.buffer_offset)
            {
                LOG_WARNING(Service_NVDRV, "Cannot remap a partially mapped GPU address space region: 0x{:X}", params.offset);
                return NvResult::BadValue;
            }

            u64 gpu_address{static_cast<u64>(params.offset + params.buffer_offset)};
            VAddr device_address{mapping->ptr + params.buffer_offset};

            system.GetVideo().MapBufferEx(gmmu, gpu_address, device_address, params.mapping_size, params.kind, mapping->big_page);
            return NvResult::Success;
        } 
        catch (const std::out_of_range&) 
        {
            LOG_WARNING(Service_NVDRV, "Cannot remap an unmapped GPU address space region: 0x{:X}", params.offset);
            return NvResult::BadValue;
        }
    }

    auto handle{nvmap.GetHandle(params.handle)};
    if (!handle)
    {
        LOG_WARNING(Service_NVDRV, "MapBufferEx uses invalid nvmap handle=0x{:X}",
                    params.handle);
        return NvResult::BadValue;
    }

    const u64 size{params.mapping_size ? params.mapping_size : handle->orig_size};
    // PinHandle maps the handle's aligned_size into the device address space. Explicit mappings
    // may therefore legitimately cover alignment padding beyond the page-aligned logical size.
    // Validate against the actually pinned extent, not handle->size.
    if (!handle->allocated || size == 0 || params.buffer_offset > handle->aligned_size ||
        size > handle->aligned_size - params.buffer_offset ||
        !Common::IsAligned(params.buffer_offset, VM::YUZU_PAGESIZE))
    {
        LOG_WARNING(Service_NVDRV,
                    "MapBufferEx exceeds nvmap backing handle=0x{:X} buffer_offset=0x{:X} size=0x{:X} logical_size=0x{:X} aligned_size=0x{:X} align=0x{:X}",
                    params.handle, params.buffer_offset, size, handle->size, handle->aligned_size,
                    handle->align);
        return NvResult::BadValue;
    }

    // MAP_BUFFER_EX page_size is an input/output field. A non-zero value is an explicit
    // page-size request; only zero asks the driver to choose the best fit from the nvmap
    // allocation. libnx and nvgpu both rely on this distinction.
    bool big_page{};
    if (params.page_size == 0)
    {
        if (Common::IsAligned(handle->align, vm.big_page_size) &&
            Common::IsAligned(params.buffer_offset, vm.big_page_size))
        {
            big_page = true;
        }
        else if (Common::IsAligned(handle->align, VM::YUZU_PAGESIZE))
        {
            big_page = false;
        }
        else
        {
            ASSERT(false);
            return NvResult::BadValue;
        }
    }
    else if (params.page_size == VM::YUZU_PAGESIZE)
    {
        if (!Common::IsAligned(handle->align, VM::YUZU_PAGESIZE))
        {
            LOG_WARNING(Service_NVDRV,
                        "MapBufferEx cannot honor explicit 4K page request for handle=0x{:X} align=0x{:X}",
                        params.handle, handle->align);
            return NvResult::BadValue;
        }
        big_page = false;
    }
    else if (params.page_size == vm.big_page_size)
    {
        if (!Common::IsAligned(handle->align, vm.big_page_size) ||
            !Common::IsAligned(params.buffer_offset, vm.big_page_size))
        {
            LOG_WARNING(Service_NVDRV,
                        "MapBufferEx cannot honor explicit big-page request for handle=0x{:X} align=0x{:X} buffer_offset=0x{:X}",
                        params.handle, handle->align, params.buffer_offset);
            return NvResult::BadValue;
        }
        big_page = true;
    }
    else
    {
        LOG_WARNING(Service_NVDRV,
                    "MapBufferEx unsupported page_size=0x{:X} handle=0x{:X}",
                    params.page_size, params.handle);
        return NvResult::BadValue;
    }

    DAddr device_address{static_cast<DAddr>(nvmap.PinHandle(params.handle, false) + params.buffer_offset)};

    if ((params.flags & MappingFlags::Fixed) != MappingFlags::None)
    {
        auto alloc{allocation_map.upper_bound(params.offset)};
        if (alloc == allocation_map.begin())
        {
            nvmap.UnpinHandle(params.handle);
            ASSERT_MSG(false, "Cannot perform a fixed mapping into an unallocated region!");
            return NvResult::BadValue;
        }
        --alloc;

        const u64 allocation_offset = params.offset - alloc->first;
        if (allocation_offset > alloc->second.size ||
            size > alloc->second.size - allocation_offset ||
            !Common::IsAligned(params.offset, VM::YUZU_PAGESIZE))
        {
            nvmap.UnpinHandle(params.handle);
            ASSERT_MSG(false, "Cannot perform a fixed mapping into an unallocated region!");
            return NvResult::BadValue;
        }

        // Each fixed mapping owns one pin and one allocation-list entry.
        // Replacing/overlapping it would lose that ownership and make unmap ambiguous.
        const auto next = mapping_map.lower_bound(params.offset);
        if ((next != mapping_map.end() && next->first - params.offset < size) ||
            (next != mapping_map.begin() &&
             params.offset - std::prev(next)->first < std::prev(next)->second->size))
        {
            nvmap.UnpinHandle(params.handle);
            return NvResult::BadValue;
        }

        const bool use_big_pages = alloc->second.big_pages && big_page &&
            Common::IsAligned(params.offset, vm.big_page_size) &&
            Common::IsAligned(size, vm.big_page_size);
        if (params.page_size == vm.big_page_size && !use_big_pages)
        {
            nvmap.UnpinHandle(params.handle);
            LOG_WARNING(Service_NVDRV,
                        "MapBufferEx explicit big-page request is incompatible with allocation offset=0x{:X} size=0x{:X} allocation_big_pages={} handle=0x{:X}",
                        params.offset, size, alloc->second.big_pages, params.handle);
            return NvResult::BadValue;
        }
        system.GetVideo().MapBufferEx(gmmu, params.offset, device_address, size, params.kind, use_big_pages);
        auto mapping{std::make_shared<Mapping>(params.handle, device_address, params.offset, size, true, use_big_pages, alloc->second.sparse)};
        alloc->second.mappings.push_back(mapping);
        mapping->allocation_entry = std::prev(alloc->second.mappings.end());
        mapping_map.emplace_hint(next, params.offset, mapping);
        if (alloc->second.sparse) {
            ReplaceSparseOwnershipLocked(alloc->second, params.offset, size, nullptr);
        }
    }
    else
    {
        auto& allocator{big_page ? *vm.big_page_allocator : *vm.small_page_allocator};
        u32 page_size{big_page ? vm.big_page_size : VM::YUZU_PAGESIZE};
        u32 page_size_bits{big_page ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS};

        params.offset = static_cast<u64>(allocator.Allocate(static_cast<u32>(Common::AlignUp(size, page_size) >> page_size_bits))) << page_size_bits;
        if (!params.offset)
        {
            nvmap.UnpinHandle(params.handle);
            ASSERT_MSG(false, "Failed to allocate free space in the GPU AS!");
            return NvResult::InsufficientMemory;
        }

        system.GetVideo().MapBufferEx(gmmu, params.offset, device_address, Common::AlignUp(size, page_size), params.kind, big_page);
        auto mapping{std::make_shared<Mapping>(params.handle, device_address, params.offset, size, false, big_page, false)};
        mapping_map[params.offset] = mapping;
    }
    return NvResult::Success;
}

NvResult nvhost_as_gpu::UnmapBuffer(IoctlUnmapBuffer& params) {
    LOG_DEBUG(Service_NVDRV, "called, offset=0x{:X}", params.offset);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    if (!FreeMappingLocked(params.offset)) {
        LOG_DEBUG(Service_NVDRV, "Cannot unmap unknown GPU address 0x{:X}", params.offset);
    }

    return NvResult::Success;
}

NvResult nvhost_as_gpu::BindChannel(IoctlBindChannel& params)
{
    LOG_DEBUG(Service_NVDRV, "called, fd={:X}", params.fd);

    std::scoped_lock lock(mutex);
    if (!vm.initialised)
    {
        return NvResult::BadValue;
    }

    auto device = module.GetDevice<nvdevice>(params.fd);
    auto* gpu_channel_device = device ? device->AsGpuChannel() : nullptr;
    if (!gpu_channel_device)
    {
        LOG_WARNING(Service_NVDRV, "BindChannel uses invalid GPU channel fd={}", params.fd);
        return NvResult::BadParameter;
    }

    if (gpu_channel_device->address_space_bound.exchange(true, std::memory_order_acq_rel))
    {
        LOG_WARNING(Service_NVDRV, "GPU channel fd={} is already bound to an address space",
                    params.fd);
        return NvResult::BadParameter;
    }

    gpu_channel_device->channel_state->SetMemoryManager(gmmu);
    has_bound_channels = true;
    return NvResult::Success;
}

void nvhost_as_gpu::GetVARegionsImpl(IoctlGetVaRegions& params)
{
    params.buf_size = 2 * sizeof(VaRegion);

    params.regions = std::array<VaRegion, 2>{
        VaRegion{
            .offset = static_cast<u64>(vm.small_page_allocator->GetVAStart()) << VM::PAGE_SIZE_BITS,
            .page_size = VM::YUZU_PAGESIZE,
            ._pad0_{},
            .pages = vm.small_page_allocator->GetVALimit() - vm.small_page_allocator->GetVAStart(),
        },
        VaRegion{
            .offset = static_cast<u64>(vm.big_page_allocator->GetVAStart()) << vm.big_page_size_bits,
            .page_size = vm.big_page_size,
            ._pad0_{},
            .pages = vm.big_page_allocator->GetVALimit() - vm.big_page_allocator->GetVAStart(),
        },
    };
}

NvResult nvhost_as_gpu::GetVARegions1(IoctlGetVaRegions& params)
{
    LOG_DEBUG(Service_NVDRV, "called, buf_addr={:X}, buf_size={:X}", params.buf_addr, params.buf_size);

    std::scoped_lock lock(mutex);

    if (!vm.initialised)
    {
        return NvResult::BadValue;
    }

    GetVARegionsImpl(params);

    return NvResult::Success;
}

NvResult nvhost_as_gpu::GetVARegions3(IoctlGetVaRegions& params, std::span<VaRegion> regions)
{
    LOG_DEBUG(Service_NVDRV, "called, buf_addr={:X}, buf_size={:X}", params.buf_addr, params.buf_size);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    GetVARegionsImpl(params);

    const size_t num_regions = std::min(params.regions.size(), regions.size());
    for (size_t i = 0; i < num_regions; i++) {
        regions[i] = params.regions[i];
    }

    return NvResult::Success;
}

Kernel::KEvent* nvhost_as_gpu::QueryEvent(u32 event_id) {
    LOG_CRITICAL(Service_NVDRV, "Unknown AS GPU Event {}", event_id);
    return nullptr;
}

} // namespace Service::Nvidia::Devices
