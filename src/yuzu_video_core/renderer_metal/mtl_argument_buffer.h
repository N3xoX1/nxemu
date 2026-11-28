// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/renderer_metal/mtl_encoder.h"
#include "yuzu_video_core/renderer_metal/mtl_shader_util.h"
#include "yuzu_video_core/renderer_metal/mtl_staging_buffer_pool.h"

namespace Metal {
class Device;
class Scheduler;

// Builds the per-draw argument buffer, replacing vk_update_descriptor and
// vk_descriptor_pool. A Tier 2 argument buffer is just an MTLBuffer of 64-bit
// handles we write ourselves, no MTLArgumentEncoder involved.
//
// Slot order here has to match what SPIRV-Cross assigned. A mismatch doesn't
// fail validation, it silently samples the wrong resource. Nothing pins the
// layout, so it's convention: the SPIR-V emitter walks one dense binding
// counter in the order uniform buffers, storage buffers, texture buffers,
// image buffers, textures, images (see spirv_emit_context.cpp) and we append
// in that same order. force_active_argument_buffer_resources keeps SPIRV-Cross
// from dropping resources it thinks are unused, and the rasterizer checks the
// slot count against ExpectedArgumentSlots. A permutation of the same count
// gets past both.
//
// Residency is not handled here. Metal 4 has no per-encoder residency and no
// automatic hazard tracking, so every allocation is declared once in the
// device's MTLResidencySet (see metal_device.cpp).
class ArgumentBufferBuilder : Common::NonCopyable, Common::NonMoveable {
public:
    explicit ArgumentBufferBuilder(const Device& device, Scheduler& scheduler,
                                   StagingBufferPool& staging_pool);
    ~ArgumentBufferBuilder();

    /// Discards anything accumulated and starts a new draw's bindings.
    void Reset();

    void Reserve(size_t num_slots) {
        if (slots.capacity() < num_slots) {
            slots.reserve(num_slots);
        }
    }

    void AddUniformBuffer(MTL::Buffer* buffer, u32 offset, u32 size);
    void AddStorageBuffer(MTL::Buffer* buffer, u32 offset, u32 size, bool is_written);
    void AddBufferAddress(u64 gpu_address) noexcept {
        slots.push_back(gpu_address);
    }
    void AddTexture(MTL::Texture* texture, MTL::SamplerState* sampler);
    /// AddTexture with the resource IDs already known, 0 for none.
    void AddTextureIds(u64 texture, u64 sampler) {
        slots.push_back(texture);
        slots.push_back(texture != 0 ? sampler : 0);
    }
    void AddSlot(u64 value) {
        slots.push_back(value);
    }
    void AddRawSlots(std::span<const u64> raw) {
        if (!raw.empty()) {
            const size_t old_size = slots.size();
            slots.resize(old_size + raw.size());
            std::memcpy(slots.data() + old_size, raw.data(), raw.size() * sizeof(u64));
        }
    }
    /// Appends a pre-hashed span of slots so Build() can compute the cache key
    /// incrementally without re-hashing tens of kilobytes of static bindless arrays.
    void AddRawSlots(std::span<const u64> raw, u64 precomputed_hash);
    void AddImage(MTL::Texture* texture, bool is_written);

    /// Finalises the argument buffer. Returns a staging ref to bind at
    /// BindingMap::ARGUMENT_BUFFER_INDEX, or a null buffer if nothing was bound.
    [[nodiscard]] StagingBufferRef Build();


    [[nodiscard]] bool IsEmpty() const noexcept {
        return slots.empty();
    }

    /// Slots written so far. Checked against what SPIRV-Cross reported for the
    /// stage, so a layout disagreement at least shows up as a log line.
    [[nodiscard]] u32 SlotCount() const noexcept {
        return static_cast<u32>(slots.size());
    }

private:
    /// One argument buffer slot. Buffers are referenced by GPU address rather
    /// than resource ID, which is how MSL declares them inside an argument
    /// buffer struct (`device T*`).
    void PushHandle(u64 value);

    const Device& device;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;

    /// Per-tick bump arena for per-stage argument buffers, avoiding stream-pool
    /// churn and 256B padding on unified memory (sized to hold large bindless
    /// tables such as RE Engine's 16-42 KiB descriptor arrays).
    static constexpr size_t ARENA_SIZE = 1024 * 1024;
    static constexpr size_t ARENA_ALIGNMENT = 64;

    struct Arena {
        MTL::Buffer* buffer{};
        u64 gpu_address{};
        std::span<u8> span;
        u64 base{};
        u32 used{};
        u64 tick{};
    };
    Arena arena;

    /// Direct-mapped per-tick deduplication cache for small argument buffers.
    /// Stores a CPU-cached copy of the slot handles so comparisons never read
    /// from WriteCombined upload memory. Reusing the exact same GPU address
    /// avoids arena churn, memcpy, GPU L1/L2 pollution, and redundant ObjC
    /// setVertexAddress/setFragmentAddress calls on the worker thread.
    static constexpr size_t CACHE_SIZE = 64;
    static constexpr size_t MAX_CACHED_SLOTS = 32;
    struct CacheEntry {
        u64 tick{};
        u64 hash{};
        u32 count{};
        StagingBufferRef ref{};
        std::array<u64, MAX_CACHED_SLOTS> data{};
    };
    std::array<CacheEntry, CACHE_SIZE> cache{};

    /// Secondary per-tick deduplication cache for large bindless argument
    /// buffers (Set 0 large arrays and Set 1 direct texture/sampler tables),
    /// matching Vulkan's texture_table reuse across draws.
    static constexpr size_t LARGE_CACHE_SIZE = 16;
    static constexpr size_t MAX_LARGE_CACHED_SLOTS = 16384;
    struct LargeCacheEntry {
        u64 tick{};
        u64 hash{};
        u32 count{};
        StagingBufferRef ref{};
        std::vector<u64> data;
    };
    std::array<LargeCacheEntry, LARGE_CACHE_SIZE> large_cache{};

    std::vector<u64> slots;
    u64 incremental_hash{0x9E3779B97F4A7C15ull};
    size_t hashed_upto{0};
    bool used_prehashed{false};

};

} // namespace Metal
