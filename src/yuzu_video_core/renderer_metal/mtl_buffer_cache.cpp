// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <ranges>
#include <set>
#include <span>

#include <boost/container/small_vector.hpp>

#include "yuzu_common/alignment.h"
#include "yuzu_common/div_ceil.h"
#include "yuzu_common/literals.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "video_settings.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/maxwell_to_mtl.h"
#include "yuzu_video_core/renderer_metal/mtl_buffer_cache.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_video_core/renderer_metal/mtl_shader_util.h"
#include "yuzu_video_core/texture_cache/expand_format.h"
#include "yuzu_video_core/vulkan_common/moltenvk_workarounds.h"

namespace Metal {
using namespace Common::Literals;

struct ReadbackShadowBudget {
    std::atomic<u64> used{};
    u64 capacity{};
};

struct ReadbackShadow {
    std::shared_ptr<ReadbackShadowBudget> budget;
    Scheduler* scheduler{};
    u64 bytes{};
    Owned<MTL::Buffer> buffer;
    Common::ReadbackReference reference;
    std::shared_ptr<Common::ReadbackCpuReference> cpu_reference;

    ReadbackShadow(std::shared_ptr<ReadbackShadowBudget> budget_, Scheduler* scheduler_, u64 bytes_,
                   Owned<MTL::Buffer> buffer_)
        : budget{std::move(budget_)}, scheduler{scheduler_}, bytes{bytes_},
          buffer{std::move(buffer_)},
          cpu_reference{std::make_shared<Common::ReadbackCpuReference>(bytes_)} {}

    ~ReadbackShadow() {
        if (scheduler != nullptr && buffer) {
            scheduler->KeepAlive(buffer.get(), buffer.get());
        }
        if (budget) {
            budget->used.fetch_sub(bytes, std::memory_order_relaxed);
        }
    }
};

namespace {
constexpr const char* READBACK_KERNELS_MSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct DeltaParams {
    uint words;
    uint source_offset;
    uint destination_offset;
    uint flag_offset;
    uint valid_mask[4];
};

kernel void readback_delta(device const uint* source_data [[buffer(0)]],
                           device uint* reference_data [[buffer(1)]],
                           device uint* destination_data [[buffer(2)]],
                           device uint* changed_blocks [[buffer(3)]],
                           constant DeltaParams& params [[buffer(4)]],
                           uint group_id [[threadgroup_position_in_grid]],
                           uint local_id [[thread_index_in_threadgroup]]) {
    threadgroup atomic_uint changed;
    const uint block = group_id;
    const uint begin = block * 16384u;
    const bool valid = (params.valid_mask[block / 32u] & (1u << (block % 32u))) != 0u;
    if (local_id == 0u) {
        atomic_store_explicit(&changed, valid ? 0u : 1u, memory_order_relaxed);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (valid) {
        for (uint i = local_id; i < 16384u && begin + i < params.words; i += 128u) {
            const uint at = params.source_offset + begin + i;
            if (source_data[at] != reference_data[at]) {
                atomic_store_explicit(&changed, 1u, memory_order_relaxed);
                break;
            }
        }
    }
    // Finish every reference read in this chunk before updating it in place.
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const uint is_changed = atomic_load_explicit(&changed, memory_order_relaxed);
    if (is_changed != 0u) {
        for (uint i = local_id; i < 16384u && begin + i < params.words; i += 128u) {
            const uint value = source_data[params.source_offset + begin + i];
            destination_data[params.destination_offset + begin + i] = value;
            reference_data[params.source_offset + begin + i] = value;
        }
    }
    if (local_id == 0u) {
        changed_blocks[params.flag_offset + block] = is_changed;
    }
}

struct TrackedParams {
    uint words;
    uint source_offset;
    uint destination_offset;
    uint flag_offset;
    uint first_page;
    uint head_words;
    uint page_base;
};

kernel void tracked_readback(device const uint* source_data [[buffer(0)]],
                             device uint* destination_data [[buffer(1)]],
                             device uint* written_pages [[buffer(2)]],
                             device atomic_uint* written_bits [[buffer(3)]],
                             constant TrackedParams& params [[buffer(4)]],
                             uint group_id [[threadgroup_position_in_grid]],
                             uint local_id [[thread_index_in_threadgroup]]) {
    threadgroup uint written;
    const uint page = params.page_base + group_id;
    const uint device_page = params.first_page + page;
    const uint word = device_page >> 5u;
    const uint bit = 1u << (device_page & 31u);
    if (local_id == 0u) {
        written = atomic_load_explicit(&written_bits[word], memory_order_relaxed) & bit;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const int begin = int(page * 1024u) - int(params.head_words);
    if (written != 0u) {
        for (uint i = local_id; i < 1024u; i += 256u) {
            const int at = begin + int(i);
            if (at >= 0 && uint(at) < params.words) {
                destination_data[params.destination_offset + uint(at)] =
                    source_data[params.source_offset + uint(at)];
            }
        }
    }
    if (local_id == 0u) {
        written_pages[params.flag_offset + page] = written != 0u ? 1u : 0u;
        // A page partly outside keeps its bit for the download of its other bytes.
        if (written != 0u && begin >= 0 && uint(begin) + 1024u <= params.words) {
            atomic_fetch_and_explicit(&written_bits[word], ~bit, memory_order_relaxed);
        }
    }
}
)MSL";

struct ReadbackDeltaParams {
    u32 words;
    u32 source_offset;
    u32 destination_offset;
    u32 flag_offset;
    std::array<u32, 4> valid_mask;
};

struct TrackedReadbackParams {
    u32 words;
    u32 source_offset;
    u32 destination_offset;
    u32 flag_offset;
    u32 first_page;
    u32 head_words;
    u32 page_base;
};
// Indices are absolute, `first` is folded in. Quad and quad-strip winding has to match
// the Vulkan backend's QuadArrayIndexBuffer / QuadStripIndexBuffer, or quads get culled.
void GenerateIndices(Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology, u32 first,
                     u32 count, std::span<u32> out) {
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;

    size_t at = 0;
    const auto emit = [&](u32 vertex) {
        if (at < out.size()) {
            out[at++] = first + vertex;
        }
    };

    switch (topology) {
    case Topology::Quads:
        for (u32 quad = 0; quad + 4 <= count; quad += 4) {
            for (const u32 corner : {0u, 1u, 2u, 0u, 2u, 3u}) {
                emit(quad + corner);
            }
        }
        break;
    case Topology::QuadStrip:
        for (u32 quad = 0; quad * 2 + 4 <= count; ++quad) {
            for (const u32 corner : {0u, 3u, 1u, 0u, 2u, 3u}) {
                emit(quad * 2 + corner);
            }
        }
        break;
    case Topology::TriangleFan:
    case Topology::Polygon:
        // Every triangle shares vertex 0.
        for (u32 vertex = 1; vertex + 1 < count; ++vertex) {
            emit(0);
            emit(vertex);
            emit(vertex + 1);
        }
        break;
    case Topology::LineLoop:
        // A strip plus the closing segment back to the first vertex.
        for (u32 vertex = 0; vertex < count; ++vertex) {
            emit(vertex);
        }
        emit(0);
        break;
    default:
        break;
    }
}

/// 256 satisfies both macOS (256) and Apple GPUs (4).
constexpr u32 UNIFORM_BUFFER_ALIGNMENT = 256;

constexpr u32 STORAGE_BUFFER_ALIGNMENT = 16;

/// Zero-filled stand-in for a vertex slot the guest never bound. Metal 4 binds a stream
/// as a bare address with no length, so an unfilled slot still fetches `stride * index`
/// off the end. 4 MiB covers a 16-bit index space at the ~40-byte strides Legends Z-A
/// uses; at the 4 KiB this used to be it faulted the GPU.
constexpr u64 NULL_BUFFER_SIZE = 4_MiB;

} // Anonymous namespace

bool SupportsPrimitiveRestart(Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology) {
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    switch (topology) {
    case Topology::Points:
    case Topology::Lines:
    case Topology::Triangles:
    case Topology::LinesAdjacency:
    case Topology::TrianglesAdjacency:
    case Topology::Patches:
        return false;
    default:
        return true;
    }
}

bool NeedsIndexConversion(Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology) {
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    switch (topology) {
    case Topology::Quads:
    case Topology::QuadStrip:
    case Topology::TriangleFan:
    case Topology::Polygon:
    case Topology::LineLoop:
        return true;
    default:
        return false;
    }
}

u32 ConvertedIndexCount(Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology, u32 count) {
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    switch (topology) {
    case Topology::Quads:
        return (count / 4) * 6;
    case Topology::QuadStrip:
        return count >= 4 ? ((count - 2) / 2) * 6 : 0;
    case Topology::TriangleFan:
    case Topology::Polygon:
        return count >= 3 ? (count - 2) * 3 : 0;
    case Topology::LineLoop:
        return count >= 2 ? count + 1 : 0;
    default:
        return count;
    }
}

MTL::PrimitiveType ConvertedPrimitive(Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology) {
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    return topology == Topology::LineLoop ? MTL::PrimitiveTypeLineStrip
                                          : MTL::PrimitiveTypeTriangle;
}

// ---------------------------------------------------------------------------
// Buffer
// ---------------------------------------------------------------------------

Buffer::Buffer(BufferCacheRuntime&, VideoCommon::NullBufferParams null_params)
    : VideoCommon::BufferBase(null_params), tracker{4096}, is_null{true} {}

Buffer::Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr_, u64 size_bytes_, bool)
    : VideoCommon::BufferBase(cpu_addr_, size_bytes_), device{&runtime.device},
      scheduler{&runtime.scheduler}, tracker{size_bytes_} {
    buffer = runtime.memory_allocator.CreateBuffer(size_bytes_, MemoryUsage::DeviceLocal);
    if (!buffer) {
        LOG_ERROR(Render_Metal, "Failed to allocate a {} byte guest buffer", size_bytes_);
        return;
    }
    buffer->setLabel(MTLSTR("eden guest buffer"));
    gpu_address = buffer->gpuAddress();
    mapped = static_cast<u8*>(buffer->contents());
}

Buffer::~Buffer() {
    if (scheduler == nullptr) {
        return;
    }

    for (BufferView& view : views) {
        if (view.texture) {
            scheduler->KeepAlive(view.texture.get(), view.texture.get());
        }
    }
    if (buffer) {
        // Passed as an allocation too, so the sweep drops it from the residency set.
        scheduler->KeepAlive(buffer.get(), buffer.get());
    }
}

void Buffer::MarkUsage(u64 offset, u64 size) noexcept {
    TouchUsageTick();
    tracker.Track(offset, size);
}

void Buffer::TouchUsageTick() noexcept {
    if (scheduler != nullptr) {
        const u64 tick = scheduler->CurrentTick();
        if (tick != last_usage_tick) {
            previous_usage_tick = last_usage_tick;
            last_usage_tick = tick;
            tracker.Reset();
        }
    }
}

void Buffer::MarkGpuCopy() noexcept {
    if (scheduler != nullptr) {
        last_gpu_copy_tick = scheduler->CurrentTick();
    }
}

bool Buffer::IsRegionUsed(u64 offset, u64 size) const noexcept {
    if (scheduler != nullptr && last_usage_tick != scheduler->CurrentTick()) {
        return false;
    }
    return tracker.IsUsed(offset, size);
}

bool Buffer::IsUsedInFlight() const noexcept {
    if (scheduler == nullptr) {
        return true;
    }
    const u64 tick =
        last_usage_tick != scheduler->CurrentTick() ? last_usage_tick : previous_usage_tick;
    return tick != 0 && !scheduler->IsFree(tick);
}

MTL::Texture* Buffer::View(u32 offset, u32 size, VideoCore::Surface::PixelFormat format) {
    if (!buffer || device == nullptr) {
        return nullptr;
    }

    for (auto it = views.begin(); it != views.end(); ++it) {
        if (it->offset == offset && it->size == size && it->format == format) {
            if (it != views.begin()) {
                std::rotate(views.begin(), it, it + 1);
            }
            return views.front().texture.get();
        }
    }

    const FormatInfo info = SurfaceFormat(*device, format);
    if (info.support == FormatSupport::Unsupported) {
        LOG_ERROR(Render_Metal, "Texel buffer view with unsupported format {}",
                  static_cast<u32>(format));
        return nullptr;
    }
    // A texel buffer is guest memory with no upload pass to widen the bytes, so a
    // format the texture cache expands would read every texel at the wrong stride.
    if (VideoCommon::IsPixelFormatExpanded(format)) {
        LOG_ERROR(Render_Metal, "Texel buffer view with format {}, which needs upload conversion",
                  static_cast<u32>(format));
        return nullptr;
    }

    const u32 bytes_per_element = VideoCore::Surface::BytesPerBlock(format);
    if (bytes_per_element == 0) {
        return nullptr;
    }

    const u64 buf_len = buffer->length();
    if (offset >= buf_len) {
        return nullptr;
    }
    const u64 max_bytes = buf_len - offset;
    const u32 clamped_size = static_cast<u32>(std::min<u64>(size, max_bytes));
    const u64 min_align = std::max<u64>(
        16, static_cast<u64>(
                device->GetDevice()->minimumLinearTextureAlignmentForPixelFormat(info.format)));
    u64 bytes_per_row = Common::AlignUp<u64>(clamped_size, min_align);
    if (bytes_per_row > max_bytes) {
        bytes_per_row = (max_bytes / min_align) * min_align;
    }
    const u32 width = static_cast<u32>(
        std::min<u64>(clamped_size, bytes_per_row) / bytes_per_element);
    if (width == 0 || bytes_per_row == 0) {
        return nullptr;
    }

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setTextureType(MTL::TextureTypeTextureBuffer);
    descriptor->setPixelFormat(info.format);
    descriptor->setWidth(width);
    descriptor->setHeight(1);
    descriptor->setDepth(1);
    descriptor->setMipmapLevelCount(1);
    descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    descriptor->setStorageMode(buffer->storageMode());

    Owned<MTL::Texture> texture = Adopt(buffer->newTexture(descriptor, offset, bytes_per_row));
    descriptor->release();

    if (!texture) {
        LOG_ERROR(Render_Metal, "Failed to create a texel buffer view");
        return nullptr;
    }

    // A texture over a buffer is its own MTL::Allocation, so the parent buffer being
    // resident does not cover it. Without this a shader sampling a texel buffer page
    // faults and takes out its whole submission ("innocent victim" errors).
    device->AddResidentAllocation(texture.get());

    // Bounded, or a streaming buffer viewed at a moving offset keeps one texture per slice.
    // Keep the evicted view alive and resident until the GPU finishes any submission using it.
    constexpr size_t MAX_VIEWS = 64;
    if (views.size() >= MAX_VIEWS) {
        if (scheduler != nullptr && views.back().texture) {
            scheduler->KeepAlive(views.back().texture.get(), views.back().texture.get());
        }
        views.pop_back();
    }
    views.insert(views.begin(), BufferView{
                                    .offset = offset,
                                    .size = size,
                                    .format = format,
                                    .texture = std::move(texture),
                                });
    return views.front().texture.get();
}

// ---------------------------------------------------------------------------
// BufferCacheRuntime
// ---------------------------------------------------------------------------

BufferCacheRuntime::BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_allocator_,
                                       Scheduler& scheduler_, StagingBufferPool& staging_pool_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      staging_pool{staging_pool_}, index_pass{device_, scheduler_, staging_pool_} {
    readback_shadow_budget = std::make_shared<ReadbackShadowBudget>();
    readback_shadow_budget->capacity = std::min<u64>(512ULL << 20, GetDeviceLocalMemory() / 16);
    prefetch_alignment = GetUniformBufferAlignment();
    if (device.HasUnifiedMemory()) {
        prefetch_buffer = memory_allocator.CreateBuffer(PrefetchRingSize, MemoryUsage::Upload);
        if (prefetch_buffer) {
            prefetch_buffer->setLabel(MTLSTR("eden uniform prefetch ring"));
            prefetch_gpu_address = prefetch_buffer->gpuAddress();
            prefetch_span = MemoryAllocator::Map(prefetch_buffer.get());
        }
    }
    ReserveNullBuffer();
}

BufferCacheRuntime::~BufferCacheRuntime() {
    if (prefetch_buffer) {
        scheduler.KeepAlive(prefetch_buffer.get(), prefetch_buffer.get());
    }
}

bool BufferCacheRuntime::AllocatePrefetch(u32 size, u32& offset, u8*& pointer) {
    if (prefetch_span.empty() || size == 0 || size > PrefetchRingSize / 8) {
        return false;
    }
    uint64_t start = Common::AlignUp(prefetch_head, prefetch_alignment);
    if (start % PrefetchRingSize + size > PrefetchRingSize) {
        start = Common::AlignUp(start, PrefetchRingSize);
    }
    if (start + size - prefetch_tail > PrefetchRingSize) {
        MasterSemaphore& master = scheduler.GetMasterSemaphore();
        master.Refresh();
        std::scoped_lock lock{prefetch_mutex};
        while (!prefetch_releases.empty() && master.IsFree(prefetch_releases.front().tick)) {
            prefetch_tail = std::max(prefetch_tail, prefetch_releases.front().position);
            prefetch_releases.pop_front();
        }
        if (start + size - prefetch_tail > PrefetchRingSize) {
            return false;
        }
    }
    prefetch_head = start + size;
    offset = static_cast<u32>(start % PrefetchRingSize);
    pointer = prefetch_span.data() + offset;
    return true;
}

void BufferCacheRuntime::ReleasePrefetch(uint64_t position) {
    if (position <= prefetch_released) {
        return;
    }
    prefetch_released = position;
    const u64 tick = scheduler.CurrentTick();
    std::scoped_lock lock{prefetch_mutex};
    if (!prefetch_releases.empty() && prefetch_releases.back().tick == tick) {
        prefetch_releases.back().position = position;
        return;
    }
    prefetch_releases.push_back({position, tick});
}

bool BufferCacheRuntime::EnsureReadbackPipelines() {
    if (readback_delta_pipeline && tracked_readback_pipeline) {
        return true;
    }
    if (readback_pipelines_attempted) {
        return false;
    }
    readback_pipelines_attempted = true;
    MslShader shader{
        .source = READBACK_KERNELS_MSL,
        .entry_point = "readback_delta",
        .stage = Shader::Stage::Compute,
    };
    Owned<MTL::Library> library = BuildLibrary(device, shader);
    if (!library) {
        LOG_WARNING(Render_Metal, "Failed to compile GPU readback compute kernels");
        differential_readback_enabled = false;
        return false;
    }
    readback_delta_pipeline =
        BuildComputePipeline(device, library.get(), "readback_delta", "eden readback delta");
    tracked_readback_pipeline =
        BuildComputePipeline(device, library.get(), "tracked_readback", "eden tracked readback");
    if (!readback_delta_pipeline) {
        differential_readback_enabled = false;
    }
    return readback_delta_pipeline && tracked_readback_pipeline;
}

bool BufferCacheRuntime::CanDifferentialReadback(const Buffer& buffer,
                                                 const VideoCommon::BufferCopy& copy) const {
    if (!differential_readback_enabled || buffer.Handle() == nullptr) {
        return false;
    }
    if (copy.size < (1ULL << 20) || copy.size > Common::ReadbackReference::MaxCopySize ||
        ((copy.src_offset | copy.dst_offset | copy.size) & 3) != 0) {
        return false;
    }
    if (buffer.SizeBytes() > (128ULL << 20)) {
        return false;
    }
    if (buffer.readback_shadow) {
        return true;
    }
    const u64 used = readback_shadow_budget->used.load(std::memory_order_relaxed);
    return used <= readback_shadow_budget->capacity &&
           buffer.SizeBytes() <= readback_shadow_budget->capacity - used;
}

void BufferCacheRuntime::InvalidateReadbackReference(Buffer& buffer, u64 offset, u64 size) {
    if (buffer.readback_shadow) {
        buffer.readback_shadow->reference.Invalidate(offset, size);
    }
}

std::shared_ptr<Common::ReadbackCpuReference> BufferCacheRuntime::GetReadbackCpuReference(
    const Buffer& buffer) const {
    return buffer.readback_shadow->cpu_reference;
}

bool BufferCacheRuntime::DifferentialReadback(Buffer& buffer, const StagingBufferRef& data,
                                              const StagingBufferRef& flags,
                                              const VideoCommon::BufferCopy& copy,
                                              u64 flag_offset) {
    if (!CanDifferentialReadback(buffer, copy)) {
        return false;
    }
    EnsureReadbackPipelines();
    if (!readback_delta_pipeline) {
        return false;
    }
    try {
        if (!buffer.readback_shadow) {
            const u64 bytes = buffer.SizeBytes();
            u64 used = readback_shadow_budget->used.load(std::memory_order_relaxed);
            do {
                if (used > readback_shadow_budget->capacity ||
                    bytes > readback_shadow_budget->capacity - used) {
                    return false;
                }
            } while (!readback_shadow_budget->used.compare_exchange_weak(
                used, used + bytes, std::memory_order_relaxed));
            Owned<MTL::Buffer> shadow_buf =
                memory_allocator.CreateBuffer(bytes, MemoryUsage::DeviceLocal);
            if (!shadow_buf) {
                readback_shadow_budget->used.fetch_sub(bytes, std::memory_order_relaxed);
                return false;
            }
            shadow_buf->setLabel(MTLSTR("eden readback shadow"));
            try {
                buffer.readback_shadow = std::make_shared<ReadbackShadow>(
                    readback_shadow_budget, &scheduler, bytes, std::move(shadow_buf));
            } catch (...) {
                readback_shadow_budget->used.fetch_sub(bytes, std::memory_order_relaxed);
                throw;
            }
            has_readback_references = true;
        }
        auto& reference = buffer.readback_shadow->reference;
        const std::array<u32, 4> valid = reference.ValidMask(copy.src_offset, copy.size);
        const u64 src_aligned = Common::AlignDown(copy.src_offset, STORAGE_BUFFER_ALIGNMENT);
        const u64 dst_aligned = Common::AlignDown(copy.dst_offset, STORAGE_BUFFER_ALIGNMENT);
        const ReadbackDeltaParams params{
            .words = static_cast<u32>(copy.size / 4),
            .source_offset = static_cast<u32>((copy.src_offset - src_aligned) / 4),
            .destination_offset = static_cast<u32>((copy.dst_offset - dst_aligned) / 4),
            .flag_offset = static_cast<u32>((flags.offset + flag_offset) / 4),
            .valid_mask = valid,
        };
        const u32 num_groups = Common::DivCeil(params.words, 16384u);
        MTL::ComputePipelineState* const state = readback_delta_pipeline.get();
        MTL::Buffer* const src_buf = buffer.Handle();
        std::shared_ptr<ReadbackShadow> shadow = buffer.readback_shadow;
        MTL::Buffer* const dst_buf = data.buffer;
        MTL::Buffer* const flags_buf = flags.buffer;
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([state, src_buf, shadow = std::move(shadow), dst_buf, flags_buf,
                          src_aligned, dst_aligned, params, num_groups](CommandContext& context) {
            ComputeEncoder encoder = context.RequestCompute();
            if (encoder == nullptr) {
                return;
            }
            encoder->setComputePipelineState(state);
            encoder->setBuffer(src_buf, src_aligned, 0);
            encoder->setBuffer(shadow->buffer.get(), src_aligned, 1);
            encoder->setBuffer(dst_buf, dst_aligned, 2);
            encoder->setBuffer(flags_buf, 0, 3);
            encoder->setBytes(&params, sizeof(params), 4);
            encoder->dispatchThreadgroups(MTL::Size::Make(num_groups, 1, 1),
                                          MTL::Size::Make(128, 1, 1));
        });
        reference.Scheduled(copy.src_offset, copy.size);
        return true;
    } catch (const std::bad_alloc&) {
        LOG_WARNING(Render_Metal,
                    "Differential GPU readback disabled: host reference allocation failed");
        differential_readback_enabled = false;
        return false;
    }
}

MTL::Buffer* BufferCacheRuntime::WriteTrackBitmap() {
    if (!write_track_bitmap) {
        EnsureReadbackPipelines();
        constexpr u64 size = Shader::Backend::SPIRV::WRITE_TRACK_WORDS * sizeof(u32);
        write_track_bitmap = memory_allocator.CreateBuffer(size, MemoryUsage::DeviceLocal);
        if (!write_track_bitmap) {
            LOG_ERROR(Render_Metal, "Failed to allocate compute write-tracking bitmap");
            return nullptr;
        }
        write_track_bitmap->setLabel(MTLSTR("eden write track bitmap"));
        const std::span<u8> mapped = MemoryAllocator::Map(write_track_bitmap.get());
        if (!mapped.empty()) {
            std::memset(mapped.data(), 0, size);
            MemoryAllocator::FlushRange(write_track_bitmap.get(), 0, size);
        } else {
            ClearBuffer(write_track_bitmap.get(), 0, size, 0);
        }
    }
    return write_track_bitmap.get();
}

bool BufferCacheRuntime::CanTrackedReadback(const VideoCommon::BufferCopy& copy,
                                            DAddr device_addr) const {
    return write_track_bitmap && tracked_readback_pipeline &&
           ((device_addr | copy.src_offset | copy.dst_offset | copy.size) & 3) == 0;
}

void BufferCacheRuntime::PreTrackedReadbackBarrier() {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([](CommandContext& context) { context.BarrierOpenBlitIfNeeded(); });
}

void BufferCacheRuntime::PostTrackedReadbackBarrier() {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([](CommandContext& context) { context.BarrierOpenBlitIfNeeded(); });
}

void BufferCacheRuntime::TrackedReadback(Buffer& buffer, const StagingBufferRef& data,
                                         const StagingBufferRef& flags,
                                         const VideoCommon::BufferCopy& copy, u64 flag_offset,
                                         DAddr device_addr) {
    static constexpr u64 PAGE_SIZE = 4096;
    static constexpr u32 MAX_GROUPS = 65535;
    const u64 src_aligned = Common::AlignDown(copy.src_offset, STORAGE_BUFFER_ALIGNMENT);
    const u64 dst_aligned = Common::AlignDown(copy.dst_offset, STORAGE_BUFFER_ALIGNMENT);
    const u64 head = device_addr % PAGE_SIZE;
    const u32 pages = static_cast<u32>(Common::DivCeil(head + copy.size, PAGE_SIZE));
    const TrackedReadbackParams params{
        .words = static_cast<u32>(copy.size / 4),
        .source_offset = static_cast<u32>((copy.src_offset - src_aligned) / 4),
        .destination_offset = static_cast<u32>((copy.dst_offset - dst_aligned) / 4),
        .flag_offset = static_cast<u32>((flags.offset + flag_offset) / 4),
        .first_page = static_cast<u32>(device_addr / PAGE_SIZE),
        .head_words = static_cast<u32>(head / 4),
        .page_base = 0,
    };
    MTL::ComputePipelineState* const state = tracked_readback_pipeline.get();
    MTL::Buffer* const src_buf = buffer.Handle();
    MTL::Buffer* const dst_buf = data.buffer;
    MTL::Buffer* const flags_buf = flags.buffer;
    MTL::Buffer* const bitmap_buf = write_track_bitmap.get();
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([state, src_buf, dst_buf, flags_buf, bitmap_buf, src_aligned, dst_aligned,
                      params, pages](CommandContext& context) {
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }
        encoder->setComputePipelineState(state);
        encoder->setBuffer(src_buf, src_aligned, 0);
        encoder->setBuffer(dst_buf, dst_aligned, 1);
        encoder->setBuffer(flags_buf, 0, 2);
        encoder->setBuffer(bitmap_buf, 0, 3);
        for (u32 base = 0; base < pages; base += MAX_GROUPS) {
            TrackedReadbackParams chunk = params;
            chunk.page_base = base;
            encoder->setBytes(&chunk, sizeof(chunk), 4);
            encoder->dispatchThreadgroups(MTL::Size::Make(std::min(pages - base, MAX_GROUPS), 1, 1),
                                          MTL::Size::Make(256, 1, 1));
        }
    });
}

void BufferCacheRuntime::ReserveNullBuffer() {
    null_buffer = memory_allocator.CreateBuffer(NULL_BUFFER_SIZE, MemoryUsage::DeviceLocal);
    if (!null_buffer) {
        throw Exception("Failed to allocate the null buffer");
    }
    null_buffer->setLabel(MTLSTR("eden null buffer"));
    null_buffer_addr = null_buffer->gpuAddress();

    write_sink = memory_allocator.CreateBuffer(NULL_BUFFER_SIZE, MemoryUsage::DeviceLocal);
    if (!write_sink) {
        throw Exception("Failed to allocate the null write sink");
    }
    write_sink->setLabel(MTLSTR("eden null write sink"));
    write_sink_addr = write_sink->gpuAddress();

    const std::span<u8> mapped = MemoryAllocator::Map(null_buffer.get());
    if (!mapped.empty()) {
        std::fill(mapped.begin(), mapped.end(), u8{0});
        MemoryAllocator::FlushRange(null_buffer.get(), 0, NULL_BUFFER_SIZE);
    } else {
        ClearBuffer(null_buffer.get(), 0, NULL_BUFFER_SIZE, 0);
    }

    const u64 min_align = std::max<u64>(
        16, static_cast<u64>(device.GetDevice()->minimumLinearTextureAlignmentForPixelFormat(
                MTL::PixelFormatRGBA32Uint)));
    MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
    desc->setTextureType(MTL::TextureTypeTextureBuffer);
    desc->setPixelFormat(MTL::PixelFormatRGBA32Uint);
    desc->setWidth(1);
    desc->setHeight(1);
    desc->setDepth(1);
    desc->setMipmapLevelCount(1);
    desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
    desc->setStorageMode(null_buffer->storageMode());
    null_texel_view = Adopt(null_buffer->newTexture(desc, 0, min_align));
    desc->release();
    if (null_texel_view) {
        null_texel_view->setLabel(MTLSTR("eden null texel view"));
        device.AddResidentAllocation(null_texel_view.get());
    }
}

void BufferCacheRuntime::ExtendUniformBinding(BufferBinding& binding, u32 shader_size) {
    // No-op on purpose. Three fixes were tried and all were worse: refilling from guest
    // memory throws away GPU writes, a GPU blit ends the open render pass once per
    // constant buffer per draw, and widening in place assumes the following bytes belong
    // to the same guest buffer. Don't retry one without knowing what those bytes are.
    (void)binding;
    (void)shader_size;
}

void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& slot_buffers) noexcept {
    // Do not call ResetUsageTracking() here: TickFrame can run mid-submission
    // and clearing the current tick's UsageTracker would let CanReorderUpload
    // overwrite ranges the open submission still reads. MarkUsage() already
    // resets the tracker whenever the submission tick advances.
    (void)slot_buffers;
    ResetDrawBindings();
}

u64 BufferCacheRuntime::CurrentTick() const noexcept {
    return scheduler.CurrentTick();
}

u64 BufferCacheRuntime::KnownGpuTick() const noexcept {
    return scheduler.GetMasterSemaphore().KnownGpuTick();
}

void BufferCacheRuntime::Wait(u64 buffer_tick) {
    scheduler.Wait(buffer_tick);
}

bool BufferCacheRuntime::IsFree(u64 tick) const {
    return scheduler.IsFree(tick);
}

void BufferCacheRuntime::Finish() {
    scheduler.Finish();
}

u64 BufferCacheRuntime::GetDeviceLocalMemory() const {
    constexpr u64 GiB = u64{1} << 30;
    const u64 working_set = device.GetRecommendedWorkingSetSize();
    const u64 scale = (std::max<u64>)(1, videoSettings.resolution_info.ScaleUp(1));
    const u64 mode_cap = videoSettings.vram_usage_mode == VramUsageMode::Aggressive
                             ? (6 * GiB + scale * GiB)
                             : (2 * GiB + scale * GiB);
    const u64 cap = device.HasUnifiedMemory() ? (std::min)(working_set / 2, mode_cap) : mode_cap;
    return (std::min)(working_set, (std::max)(cap, 2 * GiB));
}

u64 BufferCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDevice()->currentAllocatedSize();
}

bool BufferCacheRuntime::CanReportMemoryUsage() const {
    return true;
}

u32 BufferCacheRuntime::GetUniformBufferAlignment() const {
    return UNIFORM_BUFFER_ALIGNMENT;
}

u32 BufferCacheRuntime::GetStorageBufferAlignment() const {
    return STORAGE_BUFFER_ALIGNMENT;
}

bool BufferCacheRuntime::PrefersDirectUpload() const {
    return memory_allocator.PrefersDirectUpload();
}

StagingBufferRef BufferCacheRuntime::UploadStagingBuffer(size_t size) {
    return staging_pool.Request(size, MemoryUsage::Upload);
}

StagingBufferRef BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging_pool.Request(size, MemoryUsage::Download, deferred);
}

void BufferCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    staging_pool.FreeDeferred(ref);
}

bool BufferCacheRuntime::CanReorderUpload(const Buffer& buffer,
                                          std::span<const VideoCommon::BufferCopy> copies) {
    if (videoSettings.disable_buffer_reorder) {
        return false;
    }
    // Only safe when nothing in the submission being recorded reads the destination range.
    // WorkerBeginUpload() already emits a queue-level barrier before reordered copies.
    return std::ranges::none_of(copies, [&buffer](const VideoCommon::BufferCopy& copy) {
        return buffer.IsRegionUsed(copy.dst_offset, copy.size);
    });
}

void BufferCacheRuntime::PreCopyBarrier() {
    // CommitAsyncFlushesHigh issues a batch of CopyBuffer(..., barrier=false).
    // If a Metal 4 blit/compute encoder is already open, emit one barrier up
    // front so prior writes complete before the batch reads them. If in a render
    // pass or outside any encoder, the first CopyBuffer will open a fresh blit
    // encoder which already emits a queue-scope barrier.
    scheduler.Record([](CommandContext& context) { context.BarrierOpenBlitIfNeeded(); });
}

void BufferCacheRuntime::PostCopyBarrier() {
    // Empty on purpose: every subsequent non-batch CopyBuffer passes barrier=true,
    // RequestCompute always emits an intra-encoder barrier, and RequestRender
    // closes the blit encoder and emits a queue-scope barrier.
}

bool BufferCacheRuntime::CanDirectUpload(const Buffer& buffer) const noexcept {
    if (!memory_allocator.PrefersDirectUpload() || buffer.Mapped() == nullptr) {
        return false;
    }
    // +1 tick margin (matching Scheduler::KeepAlive and StagingBufferPool::TICK_MARGIN):
    // a concurrent Flush() between MarkUsage()/MarkGpuCopy() and Record() can land the
    // GPU read/write in tick + 1, so only direct-upload once tick + 1 is also retired.
    const u64 cur_tick = scheduler.CurrentTick();
    const u64 usage_tick = buffer.LastUsageTick();
    if (usage_tick != 0 && (usage_tick + 1 >= cur_tick || !scheduler.IsFree(usage_tick + 1))) {
        return false;
    }
    const u64 copy_tick = buffer.LastGpuCopyTick();
    if (copy_tick != 0 && (copy_tick + 1 >= cur_tick || !scheduler.IsFree(copy_tick + 1))) {
        return false;
    }
    return true;
}

bool BufferCacheRuntime::CanDirectDownload(const Buffer& buffer) const noexcept {
    return memory_allocator.PrefersDirectUpload() && buffer.Mapped() != nullptr;
}

void BufferCacheRuntime::WaitForTick(u64 tick) {
    scheduler.Wait(tick);
}

void BufferCacheRuntime::CopyBuffer(MTL::Buffer* dst_buffer, MTL::Buffer* src_buffer,
                                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                                    bool can_reorder_upload) {
    if (src_buffer == nullptr || dst_buffer == nullptr || copies.empty()) {
        return;
    }

    // Metal aborts the process inside copyFromBuffer if a copy runs off either end.
    boost::container::small_vector<VideoCommon::BufferCopy, 8> owned_copies;
    owned_copies.reserve(copies.size());
    const size_t src_size = src_buffer->length();
    const size_t dst_size = dst_buffer->length();
    for (const VideoCommon::BufferCopy& copy : copies) {
        if (copy.src_offset + copy.size > src_size || copy.dst_offset + copy.size > dst_size) {
            static std::atomic<u32> rejected{0};
            const u32 n = ++rejected;
            if (n == 1 || n % 500 == 0) {
                const auto label = [](MTL::Buffer* buffer) {
                    NS::String* name = buffer->label();
                    return name != nullptr ? name->utf8String() : "<unlabelled>";
                };
                LOG_ERROR(Render_Metal,
                          "Rejected out-of-bounds buffer copy #{}: src '{}' {}+{} of {}, "
                          "dst '{}' {}+{} of {}",
                          n, label(src_buffer), copy.src_offset, copy.size, src_size,
                          label(dst_buffer), copy.dst_offset, copy.size, dst_size);
            }
            continue;
        }
        owned_copies.push_back(copy);
    }
    if (owned_copies.empty()) {
        return;
    }

    // The shared cache never flushes, Vulkan's staging memory is host coherent.
    for (const VideoCommon::BufferCopy& copy : owned_copies) {
        MemoryAllocator::FlushRange(src_buffer, copy.src_offset, copy.size);
    }

    // Reorderable uploads get their own command buffer and leave the render pass open,
    // the path below ends it. Disabling this cost Odyssey 43 fps -> 28. Re-enabled:
    // Legends Z-A's wasted shadow map tile traffic halved, 26.0 -> 12.6 GB/s and
    // 2087 -> 1058 store/reload round trips a second, with no frame rate change.
    if (can_reorder_upload) {
        scheduler.RecordUpload(
            [src_buffer, dst_buffer, copies = std::move(owned_copies)](
                Scheduler& self, MTL4::ComputeCommandEncoder* encoder) {
                for (const VideoCommon::BufferCopy& copy : copies) {
                    self.EnsureUploadOrder(encoder, dst_buffer, copy.dst_offset,
                                           copy.dst_offset + copy.size);
                    encoder->copyFromBuffer(src_buffer, copy.src_offset, dst_buffer,
                                            copy.dst_offset, copy.size);
                }
            });
        return;
    }

    // Without this the recording thread's mirror still reports a pass open.
    scheduler.MarkBufferWrite(dst_buffer);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([src_buffer, dst_buffer, barrier,
                      copies = std::move(owned_copies)](CommandContext& context) {
        BlitEncoder encoder = context.RequestBlit(barrier);
        if (encoder == nullptr) {
            return;
        }
        for (const VideoCommon::BufferCopy& copy : copies) {
            encoder->copyFromBuffer(src_buffer, copy.src_offset, dst_buffer, copy.dst_offset,
                                    copy.size);
        }
    });
}

void BufferCacheRuntime::MarkHostWrite(const Buffer& buffer) {
    scheduler.MarkBufferWrite(buffer.Handle());
}

void BufferCacheRuntime::ClearBuffer(MTL::Buffer* dest_buffer, u32 offset, size_t size,
                                     u32 value) {
    if (dest_buffer == nullptr || size == 0) {
        return;
    }

    scheduler.MarkBufferWrite(dest_buffer);

    // A blit fill takes a byte pattern, anything else goes through the compute fill.
    const u8 byte_value = static_cast<u8>(value & 0xFF);
    const bool uniform_bytes = (value & 0xFF) == ((value >> 8) & 0xFF) &&
                               (value & 0xFF) == ((value >> 16) & 0xFF) &&
                               (value & 0xFF) == ((value >> 24) & 0xFF);

    const size_t words = size / sizeof(u32);
    if (!uniform_bytes && words > 0 && index_pass.IsValid()) {
        index_pass.Fill(dest_buffer, offset, static_cast<u32>(words), value);
        // The compute fill can't do a trailing partial word, guest clears are aligned.
        if (size % sizeof(u32) != 0) {
            LOG_DEBUG(Render_Metal, "Buffer clear of {} bytes has a partial trailing word", size);
        }
        return;
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dest_buffer, offset, size, byte_value](CommandContext& context) {
        BlitEncoder encoder = context.RequestBlit();
        if (encoder == nullptr) {
            return;
        }
        encoder->fillBuffer(dest_buffer, NS::Range::Make(offset, size), byte_value);
    });
}

std::span<u8> BufferCacheRuntime::BindMappedUniformBuffer(size_t stage, u32 binding_index,
                                                          u32 size) {
    // Sub-allocated out of one chunk instead of hitting the pool per binding, which once
    // measured 40% of all Draw time. The chunk MUST be dropped when the submission tick
    // changes, or a write into it can race a region the pool has reissued.
    const u32 aligned = Common::AlignUp(size, UNIFORM_BUFFER_ALIGNMENT);
    const u64 tick = scheduler.CurrentTick();

    if (uniform_arena.buffer == nullptr || uniform_arena.tick != tick ||
        uniform_arena.used + aligned > uniform_arena.span.size()) {
        const size_t chunk = (std::max)(UNIFORM_ARENA_SIZE, static_cast<size_t>(aligned));
        const StagingBufferRef ref = staging_pool.Request(chunk, MemoryUsage::Upload);
        if (ref.buffer == nullptr || ref.mapped_span.size() < aligned) {
            // The pool could not serve the chunk, so bind directly.
            const StagingBufferRef single = staging_pool.Request(size, MemoryUsage::Upload);
            BindBuffer(single.buffer, static_cast<u32>(single.offset), size);
            return single.mapped_span;
        }
        uniform_arena = UniformArena{
            .buffer = ref.buffer,
            .gpu_address = ref.gpu_address - ref.offset,
            .span = ref.mapped_span,
            .base = static_cast<u32>(ref.offset),
            .used = 0,
            .tick = tick,
        };
    }

    const u32 at = uniform_arena.used;
    uniform_arena.used += aligned;
    // Stream buffer slots and Metal's constant buffers are both 256-aligned.
    BindBuffer(uniform_arena.buffer, uniform_arena.base + at, size, false,
               uniform_arena.gpu_address);
    return uniform_arena.span.subspan(at, size);
}

void BufferCacheRuntime::BindBuffer(MTL::Buffer* buffer, u32 offset, u32 size, bool is_written,
                                    u64 gpu_address) {
    MTL::Buffer* const null_target = is_written ? write_sink.get() : null_buffer.get();
    const u64 null_addr = is_written ? write_sink_addr : null_buffer_addr;
    pending_bindings.push_back(BufferBinding{
        .buffer = buffer != nullptr ? buffer : null_target,
        .gpu_address = buffer != nullptr ? gpu_address : null_addr,
        .offset = buffer != nullptr ? offset : 0,
        .size = buffer != nullptr ? size : static_cast<u32>(NULL_BUFFER_SIZE),
        .is_written = is_written,
    });
}

void BufferCacheRuntime::BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                                           VideoCore::Surface::PixelFormat format) {
    // MSL declares a texel buffer as `texture_buffer<>`, so the argument buffer wants a
    // texture handle. Not the texture cache's image view for the same descriptor.
    // Metal silently rounds the offset of a buffer-backed texture down to its
    // alignment. The read view starts at the aligned offset instead, and a second
    // view with the size the guest bound tells the shader how many elements to
    // skip (Shader::Profile::texel_buffer_size_view). Both are bound, in that order.
    const u32 texel_size = VideoCore::Surface::BytesPerBlock(format);
    u32 alignment = 16;
    const FormatInfo info = SurfaceFormat(device, format);
    if (info.support != FormatSupport::Unsupported) {
        const u32 fmt_id = static_cast<u32>(info.format);
        if (fmt_id < linear_texture_alignments.size()) {
            u16& cached = linear_texture_alignments[fmt_id];
            if (cached == 0) {
                cached = static_cast<u16>(
                    device.GetDevice()->minimumLinearTextureAlignmentForPixelFormat(info.format));
            }
            alignment = cached;
        } else {
            alignment = static_cast<u32>(
                device.GetDevice()->minimumLinearTextureAlignmentForPixelFormat(info.format));
        }
    }
    const Vulkan::MoltenVK::TexelBufferRange range =
        Vulkan::MoltenVK::AlignedTexelBufferRange(alignment, offset, size, texel_size);
    MTL::Texture* const read_view = buffer.View(range.offset, range.size, format);
    MTL::Texture* const size_view = range.size_view_size == range.size
                                        ? read_view
                                        : buffer.View(range.offset, range.size_view_size, format);
    texel_views.push_back(read_view);
    texel_views.push_back(size_view);
}

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format,
                                         u32 first_index, u32 num_indices, MTL::Buffer* buffer,
                                         u32 offset, u32 size) {
    BindIndexBufferImpl(topology, index_format, first_index, num_indices, buffer, 0, offset, size);
}

ConvertedIndices BufferCacheRuntime::ConvertIndicesOnHost(PrimitiveTopology topology,
                                                          IndexFormat index_format,
                                                          u32 num_indices, MTL::Buffer* buffer,
                                                          u32 source_offset) {
    // Same output as IndexPass::Convert, corner order included: reversing it flips
    // the facing and the quad is culled. Line loops stay on the GPU.
    u32 primitives = 0;
    u32 verts = 0;
    switch (topology) {
    case PrimitiveTopology::Quads:
        primitives = num_indices / 4;
        verts = 6;
        break;
    case PrimitiveTopology::QuadStrip:
        primitives = num_indices >= 4 ? (num_indices - 2) / 2 : 0;
        verts = 6;
        break;
    case PrimitiveTopology::TriangleFan:
    case PrimitiveTopology::Polygon:
        primitives = num_indices >= 3 ? num_indices - 2 : 0;
        verts = 3;
        break;
    default:
        return {};
    }
    // Large draws are cheaper on the GPU than on this thread.
    static constexpr u32 MAX_HOST_OUTPUT = 1u << 18;
    const u32 output = primitives * verts;
    if (primitives == 0 || output > MAX_HOST_OUTPUT) {
        return {};
    }
    const u32 index_size = index_format == IndexFormat::UnsignedByte    ? 1
                           : index_format == IndexFormat::UnsignedShort ? 2
                                                                        : 4;
    const u8* const base = static_cast<const u8*>(buffer->contents());
    if (base == nullptr ||
        u64{source_offset} + u64{num_indices} * index_size > buffer->length()) {
        return {};
    }
    const size_t bytes = size_t{output} * sizeof(u32);
    const StagingBufferRef ref = staging_pool.Request(bytes, MemoryUsage::Upload);
    if (ref.buffer == nullptr || ref.mapped_span.size() < bytes || (ref.offset & 3u) != 0) {
        return {};
    }
    const u8* const src = base + source_offset;
    const auto read = [src, index_size](u32 at) -> u32 {
        switch (index_size) {
        case 1:
            return src[at];
        case 2: {
            u16 value;
            std::memcpy(&value, src + size_t{at} * 2, sizeof(value));
            return value;
        }
        default: {
            u32 value;
            std::memcpy(&value, src + size_t{at} * 4, sizeof(value));
            return value;
        }
        }
    };
    u32* const dst = reinterpret_cast<u32*>(ref.mapped_span.data());
    if (verts == 3) {
        const u32 first = read(0);
        for (u32 id = 0; id < primitives; ++id) {
            dst[id * 3 + 0] = first;
            dst[id * 3 + 1] = read(id + 1);
            dst[id * 3 + 2] = read(id + 2);
        }
    } else if (topology == PrimitiveTopology::Quads && index_size == 4) {
        // The common case, without the per-index switch.
        for (u32 id = 0; id < primitives; ++id) {
            u32 q[4];
            std::memcpy(q, src + size_t{id} * 16, sizeof(q));
            u32* const out = dst + size_t{id} * 6;
            out[0] = q[0];
            out[1] = q[1];
            out[2] = q[2];
            out[3] = q[0];
            out[4] = q[2];
            out[5] = q[3];
        }
    } else {
        static constexpr std::array<u32, 6> QUAD_CORNERS{0, 1, 2, 0, 2, 3};
        static constexpr std::array<u32, 6> STRIP_CORNERS{0, 3, 1, 0, 2, 3};
        const bool quads = topology == PrimitiveTopology::Quads;
        const auto& corners = quads ? QUAD_CORNERS : STRIP_CORNERS;
        const u32 stride = quads ? 4 : 2;
        for (u32 id = 0; id < primitives; ++id) {
            for (u32 k = 0; k < 6; ++k) {
                dst[size_t{id} * 6 + k] = read(id * stride + corners[k]);
            }
        }
    }
    MemoryAllocator::FlushRange(ref.buffer, ref.offset, bytes);
    return ConvertedIndices{
        .buffer = ref.buffer,
        .offset = static_cast<u32>(ref.offset),
        .num_indices = output,
        .valid = true,
    };
}

void BufferCacheRuntime::BindIndexBufferImpl(PrimitiveTopology topology, IndexFormat index_format,
                                             u32 first_index, u32 num_indices, MTL::Buffer* buffer,
                                             u64 gpu_address, u32 offset, u32 size,
                                             bool cpu_readable) {
    index_binding = IndexBufferBinding{};
    if (buffer == nullptr) {
        return;
    }

    const u32 index_size = index_format == IndexFormat::UnsignedByte    ? 1
                           : index_format == IndexFormat::UnsignedShort ? 2
                                                                        : 4;
    // Applied to the source, because a converted buffer starts at its own index zero.
    const u32 source_offset = offset + first_index * index_size;

    const auto bind_converted = [&](const ConvertedIndices& converted) {
        if (!converted.valid) {
            return false;
        }
        index_binding.buffer = converted.buffer;
        index_binding.gpu_address = converted.buffer != nullptr ? converted.buffer->gpuAddress() : 0;
        index_binding.offset = converted.offset;
        index_binding.type = MTL::IndexTypeUInt32;
        index_binding.num_indices = converted.num_indices;
        index_binding.converted = true;
        index_binding.valid = true;
        return true;
    };

    // The guest's indices may still have writes in flight, so a compute pass expands them.
    if (NeedsIndexConversion(topology)) {
        // When no GPU write to the source is in flight, expand on the host instead. The
        // compute pass ends the open render encoder, a tile store and reload of every
        // attachment, and quad-heavy titles (Fast RMX) issue ~200 of them a frame.
        if (cpu_readable && bind_converted(ConvertIndicesOnHost(topology, index_format,
                                                                num_indices, buffer,
                                                                source_offset))) {
            return;
        }
        if (!bind_converted(index_pass.Convert(topology, index_format, num_indices, 0, buffer,
                                               source_offset))) {
            LOG_WARNING(Render_Metal, "Could not convert indexed topology {}",
                        static_cast<u32>(topology));
        }
        return;
    }

    // Metal always restarts at the index type's maximum and offers no register to move it
    // or switch it off, so any other guest restart index means rewriting to 32-bit.
    const u32 metal_restart =
        index_format == IndexFormat::UnsignedShort ? 0xFFFFu : 0xFFFFFFFFu;
    const bool topology_restarts = SupportsPrimitiveRestart(topology);
    // 0xFFFFFFFF while drawing 16-bit indices means "never restart", and guests do it.
    const u32 format_max = index_format == IndexFormat::UnsignedShort ? 0xFFFFu
                           : index_format == IndexFormat::UnsignedByte ? 0xFFu
                                                                       : 0xFFFFFFFFu;
    const bool restart_reachable = restart_index <= format_max;
    // Every one of these conditions matters: rewriting an index buffer costs a compute
    // dispatch, which ends the open render encoder and forces a store and reload of every
    // attachment. Widening one that did not need it cost ~170 dispatches per submission.
    const bool needs_restart_remap = restart_enabled && topology_restarts &&
                                     restart_reachable &&
                                     index_format != IndexFormat::UnsignedByte &&
                                     restart_index != metal_restart;

    // Restart turned OFF is its own problem: Metal can't turn it off, so a 16-bit strip
    // still gets cut at a legitimate vertex 0xFFFF. Widening to 32-bit fixes that.
    const bool needs_restart_disable_widen = !restart_enabled && topology_restarts &&
                                             index_format == IndexFormat::UnsignedShort;

    // A LIST topology with restart enabled is the case both branches above miss, and the
    // one that faults the GPU. Metal restarts on strips and fans only, so a marker in a
    // list buffer is just vertex 0xFFFF, and against Legends Z-A's 36-byte stride that
    // reads 2.3 MB past a 192 KiB stream with validation staying quiet. Real hardware
    // drops the primitive holding the marker, so the widen pass collapses it to zero area.
    const u32 list_verts_per_prim =
        restart_enabled && !topology_restarts && restart_reachable
            ? IndexPass::VerticesPerPrimitive(topology)
            : 0;
    const bool needs_list_restart_scrub = list_verts_per_prim > 1;

    // Metal has no 8-bit index type, so widen.
    if (index_format == IndexFormat::UnsignedByte || needs_restart_remap ||
        needs_restart_disable_widen || needs_list_restart_scrub) {
        const u32 remap =
            restart_enabled && restart_reachable && (topology_restarts || needs_list_restart_scrub)
                ? restart_index
                : 0xFFFFFFFFu;
        if (!bind_converted(index_pass.Widen(index_format, num_indices, 0, buffer, source_offset,
                                             remap, list_verts_per_prim))) {
            LOG_WARNING(Render_Metal, "Could not widen an index buffer");
        }
        return;
    }

    // Apple Silicon's Metal 4 drawIndexedPrimitives silently drops primitives when a
    // 16-bit index buffer's GPU address is only 2-byte aligned (offset % 4 == 2), e.g.
    // BOTW's packed clipmap terrain and water sub-meshes. Realign the drawn slice into
    // a 4-byte-aligned staging buffer without splitting the active render pass.
    if (index_format == IndexFormat::UnsignedShort && (source_offset & 3u) != 0 &&
        num_indices > 0) {
        const size_t bytes = static_cast<size_t>(num_indices) * sizeof(u16);
        const u8* const src = cpu_readable ? static_cast<const u8*>(buffer->contents()) : nullptr;
        if (src != nullptr && static_cast<u64>(source_offset) + bytes <= buffer->length()) {
            const StagingBufferRef ref = staging_pool.Request(bytes, MemoryUsage::Upload);
            if (ref.buffer != nullptr && ref.mapped_span.size() >= bytes &&
                (ref.offset & 3u) == 0) {
                std::memcpy(ref.mapped_span.data(), src + source_offset, bytes);
                MemoryAllocator::FlushRange(ref.buffer, ref.offset, bytes);
                index_binding.buffer = ref.buffer;
                index_binding.gpu_address = ref.gpu_address - ref.offset;
                index_binding.offset = static_cast<u32>(ref.offset);
                index_binding.type = MTL::IndexTypeUInt16;
                index_binding.first_index_baked = true;
                index_binding.valid = true;
                return;
            }
        }
        // Widening UInt16 -> UInt32 moves Metal's fixed primitive-restart marker from
        // 0xFFFF to 0xFFFFFFFF; preserve active strip/fan restarts so 0xFFFF is not
        // fetched as vertex 65535 (e.g. RE Engine / Monster Hunter Rise UI strips).
        const u32 widen_remap =
            restart_enabled && topology_restarts && restart_reachable ? restart_index : 0xFFFFFFFFu;
        if (bind_converted(index_pass.Widen(index_format, num_indices, 0, buffer, source_offset,
                                            widen_remap, 0))) {
            return;
        }
    }

    index_binding.buffer = buffer;
    index_binding.gpu_address = gpu_address != 0 ? gpu_address : buffer->gpuAddress();
    index_binding.offset = offset;
    index_binding.type = index_format == IndexFormat::UnsignedInt ? MTL::IndexTypeUInt32
                                                                  : MTL::IndexTypeUInt16;
    index_binding.valid = true;
}

bool BufferCacheRuntime::WidensUint8IndicesOnHost() const noexcept {
    if (NeedsIndexConversion(draw_topology)) {
        return false;
    }
    // Metal restarts on strips only: a list holding a reachable marker needs the
    // index pass to collapse the primitive (see BindIndexBuffer).
    return !(restart_enabled && restart_index <= 0xFFu && !SupportsPrimitiveRestart(draw_topology));
}

void BufferCacheRuntime::BindHostUint8IndexBuffer(std::span<const u8> indices, u32 skip,
                                                  bool primitive_restart) {
    index_binding = IndexBufferBinding{};
    if (indices.empty()) {
        return;
    }
    // The draw reads from `skip` on, so the binding starts `skip` indices before the
    // widened data instead of staging the prefix. Keep the effective draw address
    // (`staging.offset + data_at`) 4-byte aligned for Metal 4 UInt16 draws.
    const size_t prefix = size_t{skip} * sizeof(u16);
    const size_t bytes = indices.size() * sizeof(u16);
    StagingBufferRef staging = staging_pool.Request(bytes, MemoryUsage::Upload);
    size_t data_at = 0;
    if (staging.buffer == nullptr || staging.mapped_span.size() < bytes ||
        staging.offset < prefix || (staging.offset & 3u) != 0) {
        const size_t aligned_prefix = Common::AlignUp(prefix, 4);
        staging = staging_pool.Request(aligned_prefix + bytes, MemoryUsage::Upload);
        if (staging.buffer == nullptr || staging.mapped_span.size() < aligned_prefix + bytes ||
            (staging.offset & 3u) != 0) {
            LOG_WARNING(Render_Metal, "No staging space for {} widened indices", indices.size());
            return;
        }
        data_at = aligned_prefix;
    }
    // Metal restarts on the type's all-ones value only, and only on strips.
    const bool remap = primitive_restart && SupportsPrimitiveRestart(draw_topology) &&
                       restart_index <= 0xFFu;
    const u8 marker = static_cast<u8>(restart_index);
    u8* const out = staging.mapped_span.data() + data_at;
    for (size_t i = 0; i < indices.size(); ++i) {
        const u16 value = remap && indices[i] == marker ? u16{0xFFFF} : u16{indices[i]};
        std::memcpy(out + i * sizeof(u16), &value, sizeof(u16));
    }
    MemoryAllocator::FlushRange(staging.buffer, staging.offset + data_at, bytes);

    index_binding.buffer = staging.buffer;
    index_binding.gpu_address = staging.gpu_address - staging.offset;
    index_binding.offset = static_cast<u32>(staging.offset + data_at - prefix);
    index_binding.type = MTL::IndexTypeUInt16;
    index_binding.valid = true;
}

void BufferCacheRuntime::BindQuadIndexBuffer(PrimitiveTopology topology, u32 first, u32 count) {
    // The shared cache only routes quads here, Vulkan draws fans and polygons natively.
    BindGeneratedIndexBuffer(topology, first, count);
}

bool BufferCacheRuntime::BindGeneratedIndexBuffer(PrimitiveTopology topology, u32 first,
                                                  u32 count) {
    index_binding = IndexBufferBinding{};

    // Guard in 64-bit against bogus/corrupted guest counts before allocating:
    // StagingBufferPool::Request throws if the power-of-two allocation exceeds
    // the device's maxBufferLength.
    static constexpr u64 MAX_GENERATED_INDICES = 16ULL * 1024 * 1024; // 64 MiB of u32 indices
    const u64 expanded_count = [&]() -> u64 {
        switch (topology) {
        case PrimitiveTopology::Quads:
            return (u64{count} / 4u) * 6u;
        case PrimitiveTopology::QuadStrip:
            return count >= 4u ? ((u64{count} - 2u) / 2u) * 6u : 0u;
        case PrimitiveTopology::TriangleFan:
        case PrimitiveTopology::Polygon:
            return count >= 3u ? (u64{count} - 2u) * 3u : 0u;
        case PrimitiveTopology::LineLoop:
            return count >= 2u ? u64{count} + 1u : 0u;
        default:
            return u64{count};
        }
    }();
    if (expanded_count == 0) {
        return false;
    }
    if (expanded_count > MAX_GENERATED_INDICES) {
        static std::atomic<u32> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 16) {
            LOG_WARNING(Render_Metal,
                        "Refusing generated index buffer for huge draw (topology={} first={} "
                        "count={} expanded={})",
                        static_cast<u32>(topology), first, count, expanded_count);
        }
        return false;
    }
    const u32 num_indices = static_cast<u32>(expanded_count);
    const size_t bytes = static_cast<size_t>(num_indices) * sizeof(u32);

    // Tried caching this by (topology, first, count) and reverted it: quad draws that vary
    // in size change the key nearly every draw, each costing a newBuffer plus a residency
    // commit. Doing it properly means one buffer plus a baseVertex, as vk_buffer_cache does.
    const StagingBufferRef ref = staging_pool.Request(bytes, MemoryUsage::Upload);
    if (ref.buffer == nullptr || ref.mapped_span.size() < bytes) {
        LOG_WARNING(Render_Metal, "No staging space for {} generated indices", num_indices);
        return false;
    }

    // The mapped span is only byte aligned, and an unaligned u32 store is undefined.
    boost::container::small_vector<u32, 256> indices(num_indices);
    GenerateIndices(topology, first, count, indices);
    std::memcpy(ref.mapped_span.data(), indices.data(), bytes);
    // Required on a discrete Mac, where Managed storage keeps separate copies.
    MemoryAllocator::FlushRange(ref.buffer, ref.offset, bytes);

    index_binding.buffer = ref.buffer;
    index_binding.gpu_address = ref.gpu_address - ref.offset;
    index_binding.offset = static_cast<u32>(ref.offset);
    // The expansion writes absolute vertex numbers, which can exceed 16 bits.
    index_binding.type = MTL::IndexTypeUInt32;
    index_binding.num_indices = num_indices;
    index_binding.converted = true;
    index_binding.folds_base_vertex = true;
    index_binding.valid = true;
    return true;
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, MTL::Buffer* buffer, u32 offset, u32 size,
                                          u32 stride) {
    if (index >= vertex_bindings.size()) {
        return;
    }
    // Vertex streams bind directly to encoder slots, not through the argument buffer.
    vertex_bindings[index] = BufferBinding{
        .buffer = buffer != nullptr ? buffer : null_buffer.get(),
        .gpu_address = buffer != nullptr ? buffer->gpuAddress() : null_buffer_addr,
        .offset = buffer != nullptr ? offset : 0,
        .size = size,
    };
}

void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    for (u32 index = 0; index < bindings.buffers.size(); ++index) {
        const u32 slot = bindings.min_index + index;
        if (slot >= vertex_bindings.size()) {
            continue;
        }
        const Buffer* const buf = bindings.buffers[index];
        MTL::Buffer* const handle = buf != nullptr ? buf->Handle() : nullptr;
        vertex_bindings[slot] = BufferBinding{
            .buffer = handle != nullptr ? handle : null_buffer.get(),
            .gpu_address = handle != nullptr ? buf->GpuAddress() : null_buffer_addr,
            .offset = handle != nullptr ? static_cast<u32>(bindings.offsets[index]) : 0u,
            .size = static_cast<u32>(bindings.sizes[index]),
        };
    }
}

void BufferCacheRuntime::BindTransformFeedbackBuffer(u32 index, MTL::Buffer* buffer, u32 offset,
                                                     u32 size) {
    // Metal has no stream-out stage, capture runs the vertex program as a kernel.
    if (index >= transform_feedback_bindings.size()) {
        return;
    }
    transform_feedback_bindings[index] = BufferBinding{
        .buffer = buffer,
        .offset = offset,
        .size = size,
    };
}

void BufferCacheRuntime::BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    // min_index is ignored on purpose. HostBindings only computes it for vertex buffers,
    // so adding it here pushes every slot out of bounds and drops all four bindings.
    for (u32 index = 0; index < bindings.buffers.size(); ++index) {
        if (bindings.buffers[index] != nullptr) {
            bindings.buffers[index]->MarkGpuCopy();
        }
        BindTransformFeedbackBuffer(index,
                                    bindings.buffers[index] ? bindings.buffers[index]->Handle()
                                                            : nullptr,
                                    static_cast<u32>(bindings.offsets[index]),
                                    static_cast<u32>(bindings.sizes[index]));
    }
}

} // namespace Metal
