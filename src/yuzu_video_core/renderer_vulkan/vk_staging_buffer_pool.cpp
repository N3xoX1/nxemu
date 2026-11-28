// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "yuzu_common/alignment.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/bit_util.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/literals.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {

using namespace Common::Literals;

// Maximum potential alignment of a Vulkan buffer
constexpr VkDeviceSize MAX_ALIGNMENT = 256;
// Stream buffer size in bytes (smaller on Android unified memory to save 64 MiB permanently)
#if defined(__ANDROID__)
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 64_MiB;
#else
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#endif
// Unused tail so a 64 KiB UBO binding near the end of the stream buffer never reads past it.
constexpr VkDeviceSize STREAM_BUFFER_TAIL = 64_KiB;

size_t GetStreamBufferSize(const Device& device) {
    VkDeviceSize size{0};
    if (device.HasDebuggingToolAttached()) {
        ForEachDeviceLocalHostVisibleHeap(device, [&size](size_t index, VkMemoryHeap& heap) {
            size = std::max(size, heap.size);
        });
        // If rebar is not supported, cut the max heap size to 40%. This will allow 2 captures to be
        // loaded at the same time in RenderDoc. If rebar is supported, this shouldn't be an issue
        // as the heap will be much larger.
        if (size <= 256_MiB) {
            size = size * 40 / 100;
        }
    } else {
        size = MAX_STREAM_BUFFER_SIZE;
    }
    return std::min(Common::AlignUp(size, MAX_ALIGNMENT), MAX_STREAM_BUFFER_SIZE);
}
} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Device& device_, MemoryAllocator& memory_allocator_,
                                     Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      stream_buffer_size{GetStreamBufferSize(device)}, region_size{stream_buffer_size /
                                                                   StagingBufferPool::NUM_SYNCS} {
    VkBufferCreateInfo stream_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = stream_buffer_size + STREAM_BUFFER_TAIL,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    stream_buffer = memory_allocator.CreateBuffer(stream_ci, MemoryUsage::Stream);
    if (device.HasDebuggingToolAttached()) {
        stream_buffer.SetObjectNameEXT("Stream Buffer");
    }
    stream_pointer = stream_buffer.Mapped();
    ASSERT_MSG(!stream_pointer.empty(), "Stream buffer must be host visible!");
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(size_t size, MemoryUsage usage, bool deferred) {
    if (!deferred && usage == MemoryUsage::Upload && size <= region_size) {
        return GetStreamBuffer(size);
    }
    return GetStagingBuffer(size, usage, deferred);
}

void StagingBufferPool::FreeDeferred(StagingBufferRef& ref) {
    auto& entries = GetCache(ref.usage)[ref.log2_level].entries;
    const auto is_this_one = [&ref](const StagingBuffer& entry) {
        return entry.index == ref.index;
    };
    auto it = std::find_if(entries.begin(), entries.end(), is_this_one);
    ASSERT(it != entries.end());
    ASSERT(it->deferred);
    it->tick = scheduler.CurrentTick();
    it->frame = frame;
    it->deferred = false;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    ReleaseCache(MemoryUsage::DeviceLocal);
    ReleaseCache(MemoryUsage::Upload);
    ReleaseCache(MemoryUsage::Download);

    static const bool profile_memory = [] {
        const char* value = std::getenv("NXEMU_MEMORY_PROFILE");
        return value != nullptr && value[0] == '1';
    }();
    if (profile_memory && frame % 300 == 0) {
        const auto held_bytes = [](const StagingBuffersCache& cache) {
            size_t bytes = 0;
            for (const auto& level : cache) {
                for (const auto& entry : level.entries) {
                    // Device-local staging buffers may not be mapped at all.
                    bytes += size_t{1} << entry.log2_level;
                }
            }
            return bytes;
        };
        LOG_INFO(Render_Vulkan,
                 "Staging memory: stream_mib={} upload_mib={} download_mib={} device_local_mib={}",
                 stream_buffer_size / 1_MiB, held_bytes(upload_cache) / 1_MiB,
                 held_bytes(download_cache) / 1_MiB, held_bytes(device_local_cache) / 1_MiB);
    }
}

StagingBufferRef StagingBufferPool::GetStreamBuffer(size_t size) {
    // Regions up to the one holding free_iterator were checked when it advanced. At the start of
    // the buffer nothing has been checked yet.
    const size_t first_unchecked_region = free_iterator == 0 ? 0 : Region(free_iterator) + 1;
    if (AreRegionsActive(first_unchecked_region,
                         std::min(Region(iterator + size) + 1, NUM_SYNCS))) {
        // Avoid waiting for the previous usages to be free
        return GetStagingBuffer(size, MemoryUsage::Upload);
    }
    const u64 current_tick = scheduler.CurrentTick();
    std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + Region(iterator),
              current_tick);
    used_iterator = iterator;
    free_iterator = std::max(free_iterator, iterator + size);

    if (iterator + size >= stream_buffer_size) {
        std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + NUM_SYNCS,
                  current_tick);
        used_iterator = 0;
        iterator = 0;
        free_iterator = size;

        if (AreRegionsActive(0, Region(size) + 1)) {
            // The start of the buffer is still in use. Leave it unchecked, otherwise the next
            // requests would be handed memory that pending copies still read from.
            free_iterator = 0;
            // Avoid waiting for the previous usages to be free
            return GetStagingBuffer(size, MemoryUsage::Upload);
        }
    }
    const size_t offset = iterator;
    iterator = Common::AlignUp(iterator + size, MAX_ALIGNMENT);
    return StagingBufferRef{
        .buffer = *stream_buffer,
        .offset = static_cast<VkDeviceSize>(offset),
        .mapped_span = stream_pointer.subspan(offset, size),
        .usage{},
        .log2_level{},
        .index{},
    };
}

bool StagingBufferPool::AreRegionsActive(size_t region_begin, size_t region_end) const {
    const u64 gpu_tick = scheduler.GetMasterSemaphore().KnownGpuTick();
    return std::any_of(sync_ticks.begin() + region_begin, sync_ticks.begin() + region_end,
                       [gpu_tick](u64 sync_tick) { return gpu_tick < sync_tick; });
};

StagingBufferRef StagingBufferPool::GetStagingBuffer(size_t size, MemoryUsage usage,
                                                     bool deferred) {
    if (const std::optional<StagingBufferRef> ref = TryGetReservedBuffer(size, usage, deferred)) {
        return *ref;
    }
    return CreateStagingBuffer(size, usage, deferred);
}

std::optional<StagingBufferRef> StagingBufferPool::TryGetReservedBuffer(size_t size,
                                                                        MemoryUsage usage,
                                                                        bool deferred) {
    const auto is_free = [this](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick);
    };
    // Reuse the oldest buffers first, so that the ones beyond what is used at once go idle.
    // A free buffer of the next size is reused rather than allocating another one.
    // A buffer in use only becomes free once the GPU completes more work, so the search
    // resumes after the buffers found in use until then.
    const u64 gpu_tick = scheduler.GetMasterSemaphore().KnownGpuTick();
    const auto find_free = [&](StagingBuffers& level) {
        if (level.scan_tick != gpu_tick) {
            level.scan_index = 0;
            level.scan_tick = gpu_tick;
        }
        const auto it = std::find_if(level.entries.begin() + level.scan_index,
                                     level.entries.end(), is_free);
        level.scan_index = std::distance(level.entries.begin(), it);
        return it;
    };
    auto& cache = GetCache(usage);
    const size_t log2 = Common::Log2Ceil64(size);
    StagingBuffers* level = &cache[log2];
    auto it = find_free(*level);
    if (it == level->entries.end() && log2 + 1 < NUM_LEVELS) {
        level = &cache[log2 + 1];
        it = find_free(*level);
    }
    if (it == level->entries.end()) {
        return std::nullopt;
    }
    ++level->scan_index;
    it->tick = deferred ? std::numeric_limits<u64>::max() : scheduler.CurrentTick();
    it->frame = frame;
    ASSERT(!it->deferred);
    it->deferred = deferred;
    return it->Ref();
}

StagingBufferRef StagingBufferPool::CreateStagingBuffer(size_t size, MemoryUsage usage,
                                                        bool deferred) {
    const u32 log2 = Common::Log2Ceil64(size);
    VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 1ULL << log2,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    vk::Buffer buffer = memory_allocator.TryCreateBuffer(buffer_ci, usage);
    if (!buffer) {
        // No memory left for it (Lichii #262: an 8 GB laptop and a 128 MiB upload). Free the idle
        // staging buffers and try again, then wait for the GPU so the ones it was still reading
        // are idle too.
        ReleaseIdleBuffers();
        buffer = memory_allocator.TryCreateBuffer(buffer_ci, usage);
        if (!buffer) {
            ReleaseMemory();
            buffer = memory_allocator.TryCreateBuffer(buffer_ci, usage);
        }
        if (!buffer) {
            LOG_CRITICAL(Render_Vulkan, "No memory for a {} byte staging buffer (usage {})", size,
                         static_cast<u32>(usage));
            throw vk::Exception(VK_ERROR_OUT_OF_DEVICE_MEMORY);
        }
        static u32 logged{};
        if (logged++ < 32) {
            LOG_WARNING(Render_Vulkan,
                        "Out of memory for a {} byte staging buffer; freed idle staging buffers",
                        size);
        }
    }
    if (device.HasDebuggingToolAttached()) {
        ++buffer_index;
        buffer.SetObjectNameEXT(fmt::format("Staging Buffer {}", buffer_index).c_str());
    }
    const std::span<u8> mapped_span = buffer.Mapped();
    StagingBuffer& entry = GetCache(usage)[log2].entries.emplace_back(StagingBuffer{
        .buffer = std::move(buffer),
        .mapped_span = mapped_span,
        .usage = usage,
        .log2_level = log2,
        .index = unique_ids++,
        .tick = deferred ? std::numeric_limits<u64>::max() : scheduler.CurrentTick(),
        .frame = frame,
        .deferred = deferred,
    });
    return entry.Ref();
}

StagingBufferPool::StagingBuffersCache& StagingBufferPool::GetCache(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
        return device_local_cache;
    case MemoryUsage::Upload:
        return upload_cache;
    case MemoryUsage::Download:
        return download_cache;
    default:
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return upload_cache;
    }
}

void StagingBufferPool::ReleaseCache(MemoryUsage usage) {
    auto& cache = GetCache(usage);
    for (size_t log2 = 0; log2 < NUM_LEVELS; ++log2) {
        ReleaseLevel(cache, log2);
    }
}

void StagingBufferPool::ReleaseIdleBuffers() {
    for (StagingBuffersCache* const cache : {&device_local_cache, &upload_cache, &download_cache}) {
        for (StagingBuffers& staging : *cache) {
            std::erase_if(staging.entries, [this](const StagingBuffer& entry) {
                return !entry.deferred && scheduler.IsFree(entry.tick);
            });
            staging.scan_index = 0;
        }
    }
}

void StagingBufferPool::ReleaseMemory() {
    // Waits for the GPU so the buffers it was still reading are idle too. Those handed out in
    // the current tick may feed commands not recorded yet (the caller's input while it asks for
    // its output): they move to the tick after Finish, so they are neither freed nor handed out
    // again.
    const u64 current_tick = scheduler.CurrentTick();
    scheduler.Finish();
    const u64 next_tick = scheduler.CurrentTick();
    for (StagingBuffersCache* const cache : {&device_local_cache, &upload_cache, &download_cache}) {
        for (StagingBuffers& staging : *cache) {
            for (StagingBuffer& entry : staging.entries) {
                if (!entry.deferred && entry.tick >= current_tick) {
                    entry.tick = next_tick;
                }
            }
        }
    }
    for (u64& sync_tick : sync_ticks) {
        if (sync_tick >= current_tick) {
            sync_tick = next_tick;
        }
    }
    ReleaseIdleBuffers();
}

void StagingBufferPool::ReleaseLevel(StagingBuffersCache& cache, size_t log2) {
    const auto is_deletable = [this](const StagingBuffer& entry) {
        const auto idle_frames = (size_t{1} << entry.log2_level) > LARGE_BUFFER_BYTES
                                     ? LARGE_IDLE_FRAMES
                                     : IDLE_FRAMES;
        return !entry.deferred && frame - entry.frame >= idle_frames &&
               scheduler.IsFree(entry.tick);
    };
    std::erase_if(cache[log2].entries, is_deletable);
    cache[log2].scan_index = 0;
}

} // namespace Vulkan
