// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstring>

#include "yuzu_common/alignment.h"
#include "yuzu_common/cityhash.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_argument_buffer.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"

namespace Metal {
ArgumentBufferBuilder::ArgumentBufferBuilder(const Device& device_, Scheduler& scheduler_,
                                             StagingBufferPool& staging_pool_)
    : device{device_}, scheduler{scheduler_}, staging_pool{staging_pool_} {
    // Avoid reallocating on the per-draw hot path.
    slots.reserve(64);
}

ArgumentBufferBuilder::~ArgumentBufferBuilder() = default;

void ArgumentBufferBuilder::Reset() {
    slots.clear();
    incremental_hash = 0x9E3779B97F4A7C15ull;
    hashed_upto = 0;
    used_prehashed = false;
}

void ArgumentBufferBuilder::AddRawSlots(std::span<const u64> raw, u64 precomputed_hash) {
    if (raw.empty()) {
        return;
    }
    if (slots.size() > hashed_upto) {
        const size_t pending_count = slots.size() - hashed_upto;
        const u64 chunk_hash = Common::CityHash64(
            reinterpret_cast<const char*>(slots.data() + hashed_upto), pending_count * sizeof(u64));
        incremental_hash = (incremental_hash ^ chunk_hash) * 0xBF58476D1CE4E5B9ull;
        incremental_hash ^= pending_count;
    }
    const size_t old_size = slots.size();
    slots.resize(old_size + raw.size());
    std::memcpy(slots.data() + old_size, raw.data(), raw.size() * sizeof(u64));
    incremental_hash = (incremental_hash ^ precomputed_hash) * 0xBF58476D1CE4E5B9ull;
    incremental_hash ^= raw.size();
    hashed_upto = slots.size();
    used_prehashed = true;
}

void ArgumentBufferBuilder::PushHandle(u64 value) {
    slots.push_back(value);
}

void ArgumentBufferBuilder::AddUniformBuffer(MTL::Buffer* buffer, u32 offset, u32 size) {
    if (buffer == nullptr) {
        // A null slot must still occupy its index, or every later binding
        // shifts and the shader reads the wrong resource.
        PushHandle(0);
        return;
    }
    PushHandle(buffer->gpuAddress() + offset);
}

void ArgumentBufferBuilder::AddStorageBuffer(MTL::Buffer* buffer, u32 offset, u32 size,
                                             bool is_written) {
    if (buffer == nullptr) {
        PushHandle(0);
        return;
    }
    PushHandle(buffer->gpuAddress() + offset);
}

void ArgumentBufferBuilder::AddTexture(MTL::Texture* texture, MTL::SamplerState* sampler) {
    // A combined image sampler takes two consecutive slots, texture then
    // sampler. See the layout contract in the header.
    if (texture == nullptr) {
        PushHandle(0);
        PushHandle(0);
        return;
    }
    PushHandle(texture->gpuResourceID()._impl);

    PushHandle(sampler != nullptr ? sampler->gpuResourceID()._impl : 0);
    // Sampler states are not MTLResources and need no residency declaration.
}

void ArgumentBufferBuilder::AddImage(MTL::Texture* texture, bool is_written) {
    if (texture == nullptr) {
        PushHandle(0);
        return;
    }
    PushHandle(texture->gpuResourceID()._impl);
}

StagingBufferRef ArgumentBufferBuilder::Build() {
    if (slots.empty()) {
        return StagingBufferRef{};
    }

    const size_t count = slots.size();
    const size_t size_bytes = count * sizeof(u64);
    const u64 current_tick = scheduler.CurrentTick();

    u64 hash = 0;
    CacheEntry* cache_entry = nullptr;
    LargeCacheEntry* large_cache_entry = nullptr;
    if (count <= MAX_LARGE_CACHED_SLOTS) {
        if (used_prehashed) {
            u64 h = incremental_hash;
            if (count > hashed_upto) {
                const size_t tail_count = count - hashed_upto;
                const u64 tail_hash = Common::CityHash64(
                    reinterpret_cast<const char*>(slots.data() + hashed_upto),
                    tail_count * sizeof(u64));
                h = (h ^ tail_hash) * 0xBF58476D1CE4E5B9ull;
                h ^= tail_count;
            }
            hash = h ^ (count * 0x94D049BB133111EBull);
        } else {
            hash = Common::CityHash64(reinterpret_cast<const char*>(slots.data()), size_bytes);
        }
        if (count <= MAX_CACHED_SLOTS) {
            cache_entry = &cache[static_cast<size_t>(hash) & (CACHE_SIZE - 1)];
            if (cache_entry->tick == current_tick && cache_entry->hash == hash &&
                cache_entry->count == count &&
                std::memcmp(cache_entry->data.data(), slots.data(), size_bytes) == 0) {
                return cache_entry->ref;
            }
        } else {
            large_cache_entry = &large_cache[static_cast<size_t>(hash) & (LARGE_CACHE_SIZE - 1)];
            if (large_cache_entry->tick == current_tick && large_cache_entry->hash == hash &&
                large_cache_entry->count == count && large_cache_entry->data.size() >= count &&
                std::memcmp(large_cache_entry->data.data(), slots.data(), size_bytes) == 0) {
                return large_cache_entry->ref;
            }
        }
    }

    const size_t align = device.HasUnifiedMemory() ? ARENA_ALIGNMENT : 256;
    const u32 aligned = static_cast<u32>(Common::AlignUp(size_bytes, align));

    StagingBufferRef result{};
    if (aligned <= ARENA_SIZE) {
        if (arena.buffer == nullptr || arena.tick != current_tick ||
            arena.used + aligned > arena.span.size()) {
            const StagingBufferRef chunk = staging_pool.Request(ARENA_SIZE, MemoryUsage::Upload);
            if (chunk.buffer != nullptr && chunk.mapped_span.size() >= ARENA_SIZE) {
                arena.buffer = chunk.buffer;
                arena.gpu_address = chunk.gpu_address;
                arena.span = chunk.mapped_span;
                arena.base = chunk.offset;
                arena.used = 0;
                arena.tick = current_tick;
            } else {
                arena = Arena{};
            }
        }
        if (arena.buffer != nullptr && arena.used + aligned <= arena.span.size()) {
            const u32 at = arena.used;
            arena.used += aligned;
            std::memcpy(arena.span.data() + at, slots.data(), size_bytes);
            MemoryAllocator::FlushRange(arena.buffer, arena.base + at, size_bytes);
            result = StagingBufferRef{
                .buffer = arena.buffer,
                .offset = arena.base + at,
                .gpu_address = arena.gpu_address + at,
                .mapped_span = arena.span.subspan(at, size_bytes),
                .usage = MemoryUsage::Upload,
            };
        }
    }

    if (result.buffer == nullptr) {
        StagingBufferRef ref = staging_pool.Request(size_bytes, MemoryUsage::Upload);
        if (ref.mapped_span.size() < size_bytes) {
            LOG_ERROR(Render_Metal, "Argument buffer staging allocation is too small");
            return StagingBufferRef{};
        }
        std::memcpy(ref.mapped_span.data(), slots.data(), size_bytes);
        MemoryAllocator::FlushRange(ref.buffer, ref.offset, size_bytes);
        result = ref;
    }

    if (cache_entry != nullptr) {
        cache_entry->tick = current_tick;
        cache_entry->hash = hash;
        cache_entry->count = static_cast<u32>(count);
        cache_entry->ref = result;
        std::memcpy(cache_entry->data.data(), slots.data(), size_bytes);
    } else if (large_cache_entry != nullptr) {
        large_cache_entry->tick = current_tick;
        large_cache_entry->hash = hash;
        large_cache_entry->count = static_cast<u32>(count);
        large_cache_entry->ref = result;
        if (large_cache_entry->data.size() < count) {
            large_cache_entry->data.resize(count);
        }
        std::memcpy(large_cache_entry->data.data(), slots.data(), size_bytes);
    }
    return result;
}

} // namespace Metal
