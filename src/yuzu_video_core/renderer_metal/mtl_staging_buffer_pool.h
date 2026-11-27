// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <climits>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/metal_common/metal_memory_allocator.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"

namespace Metal {

class Device;
class Scheduler;

/// A borrowed region of a staging buffer.
struct StagingBufferRef {
    MTL::Buffer* buffer{};
    u64 offset{};
    u64 gpu_address{};
    std::span<u8> mapped_span;
    MemoryUsage usage{};
    u32 log2_level{};
    u64 index{};
};

// Pools host-visible buffers for moving data between guest memory and GPU
// resources. A linear stream buffer for small short-lived requests, plus
// power-of-two buckets for larger ones, both fenced against the semaphore tick.
//
// On unified memory a staging copy is pure overhead. Callers that can write
// straight into the destination should ask MemoryAllocator::PrefersDirectUpload
// and skip this pool. Still needed for discrete GPUs and for readback.
class StagingBufferPool : Common::NonCopyable, Common::NonMoveable {
public:
    /// Number of independently fenced regions the stream buffer is split into.
    /// One tick tracked per region.
    static constexpr size_t NUM_SYNCS = 16;

    explicit StagingBufferPool(const Device& device, MemoryAllocator& memory_allocator,
                               Scheduler& scheduler);
    ~StagingBufferPool();

    /// Borrows a region of at least @p size bytes.
    /// @param deferred When set, the region is not recycled automatically and
    ///                 must be returned with FreeDeferred.
    [[nodiscard]] StagingBufferRef Request(size_t size, MemoryUsage usage, bool deferred = false);

    void FreeDeferred(StagingBufferRef& ref);

    [[nodiscard]] MTL::Buffer* StreamBuffer() const noexcept {
        return stream_buffer.get();
    }

    void TickFrame();

private:
    struct StreamBufferCommit {
        size_t upper_bound{};
        u64 tick{};
    };

    struct StagingBuffer {
        Owned<MTL::Buffer> buffer;
        u64 gpu_address{};
        std::span<u8> mapped_span;
        MemoryUsage usage{};
        u32 log2_level{};
        u64 index{};
        u64 tick{};
        bool deferred{};

        [[nodiscard]] StagingBufferRef Ref() const noexcept {
            return StagingBufferRef{
                .buffer = buffer.get(),
                .offset = 0,
                .gpu_address = gpu_address,
                .mapped_span = mapped_span,
                .usage = usage,
                .log2_level = log2_level,
                .index = index,
            };
        }
    };

    struct StagingBuffers {
        std::vector<StagingBuffer> entries;
        size_t delete_index{};

        [[nodiscard]] size_t Size() const noexcept {
            return entries.size();
        }
    };

    static constexpr size_t NUM_LEVELS = sizeof(size_t) * CHAR_BIT;
    using StagingBuffersCache = std::array<StagingBuffers, NUM_LEVELS>;

    [[nodiscard]] StagingBufferRef GetStreamBuffer(size_t size);
    [[nodiscard]] bool AreRegionsActive(size_t region_begin, size_t region_end) const;

    [[nodiscard]] StagingBufferRef GetStagingBuffer(size_t size, MemoryUsage usage, bool deferred);
    [[nodiscard]] std::optional<StagingBufferRef> TryGetReservedBuffer(size_t size,
                                                                      MemoryUsage usage,
                                                                      bool deferred);
    [[nodiscard]] StagingBufferRef CreateStagingBuffer(size_t size, MemoryUsage usage,
                                                       bool deferred);

    [[nodiscard]] StagingBuffersCache& GetCache(MemoryUsage usage);

    void ReleaseCache(MemoryUsage usage);
    void ReleaseLevel(StagingBuffersCache& cache, size_t log2);
    void ReleaseIdleBuffers();
    void ReleaseMemory();

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;

    Owned<MTL::Buffer> stream_buffer;
    u64 stream_gpu_address{};
    std::span<u8> stream_pointer;

    size_t iterator{};
    size_t used_iterator{};
    size_t free_iterator{};
    std::array<u64, NUM_SYNCS> sync_ticks{};

    StagingBuffersCache device_local_cache;
    StagingBuffersCache upload_cache;
    StagingBuffersCache download_cache;

    size_t current_delete_level{};
    u64 buffer_index{};
    u64 unique_ids{};
    mutable std::mutex pool_mutex;
};

} // namespace Metal
