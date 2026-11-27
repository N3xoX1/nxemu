// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>

#include "yuzu_common/alignment.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/literals.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_video_core/renderer_metal/mtl_staging_buffer_pool.h"

namespace Metal {

using namespace Common::Literals;

namespace {

/// Size of the linear stream buffer used for small, short-lived requests.
constexpr size_t STREAM_BUFFER_SIZE = 128_MiB;
constexpr size_t REGION_SIZE = STREAM_BUFFER_SIZE / StagingBufferPool::NUM_SYNCS;

/// Requests at or below this go through the stream buffer; larger ones get a
/// dedicated allocation from the bucketed cache.
constexpr size_t MAX_STREAM_BUFFER_REQUEST_SIZE = 8_MiB;

/// Buffer offsets have to satisfy the strictest alignment any consumer needs.
/// 256 covers constant buffer binding on every Metal device and is a multiple
/// of the 16-byte alignment vertex and storage buffers ask for.
constexpr size_t STREAM_BUFFER_ALIGNMENT = 256;

/// One tick of margin: Flush() can run from another thread between stamping a
/// staging region and recording the command that reads it.
constexpr u64 TICK_MARGIN = 1;

size_t Region(size_t iterator) noexcept {
    return iterator / REGION_SIZE;
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Device& device_, MemoryAllocator& memory_allocator_,
                                     Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_} {
    stream_buffer = memory_allocator.CreateBuffer(STREAM_BUFFER_SIZE, MemoryUsage::Stream);
    if (!stream_buffer) {
        throw Exception("Failed to allocate the staging stream buffer");
    }
    stream_buffer->setLabel(MTLSTR("eden stream buffer"));
    stream_gpu_address = stream_buffer->gpuAddress();

    stream_pointer = MemoryAllocator::Map(stream_buffer.get());
    if (stream_pointer.empty()) {
        throw Exception("Staging stream buffer is not host visible");
    }
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(size_t size, MemoryUsage usage, bool deferred) {
    std::scoped_lock lock{pool_mutex};
    const size_t alloc_size = std::max<size_t>(size, 1);
    if (!deferred && usage == MemoryUsage::Upload &&
        alloc_size <= MAX_STREAM_BUFFER_REQUEST_SIZE) {
        return GetStreamBuffer(alloc_size);
    }
    return GetStagingBuffer(alloc_size, usage, deferred);
}

void StagingBufferPool::FreeDeferred(StagingBufferRef& ref) {
    std::scoped_lock lock{pool_mutex};
    auto& cache = GetCache(ref.usage);
    auto& entries = cache[ref.log2_level].entries;

    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&ref](const StagingBuffer& entry) {
                                     return entry.index == ref.index;
                                 });
    ASSERT(it != entries.end());
    ASSERT(it->deferred);

    // One tick of margin. Flush() is reached from several threads, so one landing
    // between this stamp and the recording puts the commands reading this region
    // in the next submission. Any more than that starves the pool and stalls the
    // picture.
    it->tick = scheduler.CurrentTick() + TICK_MARGIN;
    it->deferred = false;
}

void StagingBufferPool::TickFrame() {
    std::scoped_lock lock{pool_mutex};
    current_delete_level = (current_delete_level + 1) % NUM_LEVELS;

    ReleaseCache(MemoryUsage::DeviceLocal);
    ReleaseCache(MemoryUsage::Upload);
    ReleaseCache(MemoryUsage::Download);
}

bool StagingBufferPool::AreRegionsActive(size_t region_begin, size_t region_end) const {
    // Clamped: an empty or backwards range means "nothing to check". Trusting
    // it would read off the end of sync_ticks.
    region_begin = std::min(region_begin, NUM_SYNCS);
    region_end = std::min(region_end, NUM_SYNCS);
    if (region_begin >= region_end) {
        return false;
    }
    return std::any_of(sync_ticks.begin() + region_begin, sync_ticks.begin() + region_end,
                       [this](u64 tick) { return !scheduler.IsFree(tick); });
}

StagingBufferRef StagingBufferPool::GetStreamBuffer(size_t size) {
    const bool wraps = iterator + size >= STREAM_BUFFER_SIZE;
    const size_t first_unchecked_region = free_iterator == 0 ? 0 : Region(free_iterator) + 1;
    // Test both the tail and (on wrap) the head BEFORE mutating any iterator or
    // sync_ticks, so a bail-out leaves the stream pool state completely untouched.
    if (AreRegionsActive(first_unchecked_region,
                         std::min(Region(iterator + size) + 1, NUM_SYNCS)) ||
        (wraps && AreRegionsActive(0, Region(size) + 1))) {
        // The write would land in a region still in flight; fall back to a
        // dedicated allocation rather than stalling the recording thread.
        return GetStagingBuffer(size, MemoryUsage::Upload, false);
    }

    // Regions handed out since the last call are consumed by the submission
    // still being recorded, or by the next one when a pass boundary flushes
    // before their commands are recorded, so they wait for that one too.
    const u64 current_tick = scheduler.CurrentTick() + TICK_MARGIN;
    std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + Region(iterator),
              current_tick);
    used_iterator = iterator;

    if (wraps) {
        // All three iterators must move together; free_iterator in particular
        // must be assigned, not raised with max(), or the guard above computes
        // a backwards range and walks off the end of sync_ticks.
        std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.end(), current_tick);
        used_iterator = 0;
        iterator = 0;
        free_iterator = size;
    } else {
        free_iterator = std::max(free_iterator, iterator + size);
    }

    const size_t offset = iterator;
    iterator = Common::AlignUp(iterator + size, STREAM_BUFFER_ALIGNMENT);

    return StagingBufferRef{
        .buffer = stream_buffer.get(),
        .offset = offset,
        .gpu_address = stream_gpu_address + offset,
        .mapped_span = stream_pointer.subspan(offset, size),
        .usage = MemoryUsage::Upload,
        .log2_level = 0,
        .index = 0,
    };
}

StagingBufferRef StagingBufferPool::GetStagingBuffer(size_t size, MemoryUsage usage,
                                                     bool deferred) {
    if (const auto ref = TryGetReservedBuffer(size, usage, deferred)) {
        return *ref;
    }
    return CreateStagingBuffer(size, usage, deferred);
}

std::optional<StagingBufferRef> StagingBufferPool::TryGetReservedBuffer(size_t size,
                                                                       MemoryUsage usage,
                                                                       bool deferred) {
    const size_t alloc_size = std::max<size_t>(size, 1);
    StagingBuffers& cache_level = GetCache(usage)[std::bit_width(alloc_size - 1)];

    const auto is_free = [this](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick);
    };

    auto it = std::find_if(cache_level.entries.begin(), cache_level.entries.end(), is_free);
    if (it == cache_level.entries.end()) {
        return std::nullopt;
    }

    // One tick of margin, as in FreeDeferred: the caller can still flush (a pass boundary
    // does) before recording the commands that use the buffer.
    it->tick = deferred ? (std::numeric_limits<u64>::max)() : scheduler.CurrentTick() + TICK_MARGIN;
    it->deferred = deferred;
    return it->Ref();
}

StagingBufferRef StagingBufferPool::CreateStagingBuffer(size_t size, MemoryUsage usage,
                                                        bool deferred) {
    const size_t alloc_size = std::max<size_t>(size, 1);
    const u32 log2 = static_cast<u32>(std::bit_width(alloc_size - 1));
    const size_t allocation_size = size_t{1} << log2;

    Owned<MTL::Buffer> buffer = memory_allocator.CreateBuffer(allocation_size, usage);
    if (!buffer) {
        throw Exception("Failed to allocate a staging buffer");
    }
    buffer->setLabel(MTLSTR("eden staging buffer"));

    std::span<u8> mapped_span = MemoryAllocator::Map(buffer.get());
    if (mapped_span.empty() && usage != MemoryUsage::DeviceLocal) {
        device.RemoveResidentAllocation(buffer.get());
        device.CommitResidency();
        // Upload and Download buffers must be host visible or they are useless.
        throw Exception("Staging buffer is not host visible");
    }

    const u64 gpu_addr = buffer->gpuAddress();
    StagingBuffers& cache_level = GetCache(usage)[log2];
    cache_level.entries.push_back(StagingBuffer{
        .buffer = std::move(buffer),
        .gpu_address = gpu_addr,
        .mapped_span = mapped_span,
        .usage = usage,
        .log2_level = log2,
        .index = unique_ids++,
        .tick = deferred ? (std::numeric_limits<u64>::max)() : scheduler.CurrentTick() + TICK_MARGIN,
        .deferred = deferred,
    });

    return cache_level.entries.back().Ref();
}

StagingBufferPool::StagingBuffersCache& StagingBufferPool::GetCache(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::Upload:
    case MemoryUsage::Stream:
        return upload_cache;
    case MemoryUsage::Download:
        return download_cache;
    case MemoryUsage::DeviceLocal:
    case MemoryUsage::Transient:
    default:
        return device_local_cache;
    }
}

void StagingBufferPool::ReleaseCache(MemoryUsage usage) {
    ReleaseLevel(GetCache(usage), current_delete_level);
}

void StagingBufferPool::ReleaseLevel(StagingBuffersCache& cache, size_t log2) {
    // Bounded per-frame retirement, so a spike's allocations do not linger for
    // a whole session.
    constexpr size_t deletions_per_tick = 16;

    StagingBuffers& staging = cache[log2];
    std::vector<StagingBuffer>& entries = staging.entries;

    const size_t old_size = entries.size();
    if (old_size == 0) {
        return;
    }

    const auto is_deletable = [this](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick);
    };

    const size_t begin = staging.delete_index;
    const size_t end = std::min(begin + deletions_per_tick, old_size);

    const auto begin_it = entries.begin() + static_cast<std::ptrdiff_t>(begin);
    const auto end_it = entries.begin() + static_cast<std::ptrdiff_t>(end);

    // Reclaimed from the residency set before anything moves, or the set grows
    // for the whole session and pins every staging buffer we ever churned against
    // eviction. Removals no longer force a requestResidency(), which is what made
    // this too expensive the first time it was tried.
    //
    // Done here instead of over [new_end, end_it) after the remove_if. remove_if
    // compacts the *kept* elements forward and leaves that tail valid but
    // unspecified, part moved-from nulls and part removed entries it never had to
    // overwrite. Reading buffers out of it missed an arbitrary subset, and the
    // erase below then destroyed those buffers with the residency set still
    // pointing at them. Republishing a set in that state faults the GPU.
    for (auto it = begin_it; it != end_it; ++it) {
        if (is_deletable(*it)) {
            device.RemoveResidentAllocation(it->buffer.get());
        }
    }

    const auto new_end = std::remove_if(begin_it, end_it, is_deletable);
    entries.erase(new_end, end_it);
    staging.delete_index += (end - begin) - (static_cast<size_t>(end_it - new_end));
    if (staging.delete_index >= entries.size()) {
        staging.delete_index = 0;
    }
}

} // namespace Metal
