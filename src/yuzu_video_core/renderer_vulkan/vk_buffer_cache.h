// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_video_core/buffer_cache/buffer_cache_base.h"
#include "yuzu_common/readback_reference.h"
#include "yuzu_video_core/buffer_cache/memory_tracker_base.h"
#include "yuzu_video_core/buffer_cache/usage_tracker.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/renderer_vulkan/vk_compute_pass.h"
#include "yuzu_video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/renderer_vulkan/vertex_input_mapping.h"
#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
class DescriptorPool;
class Scheduler;
struct HostVertexBinding;

class BufferCacheRuntime;
struct ReadbackShadow;
struct ReadbackShadowBudget;

class Buffer : public VideoCommon::BufferBase {
    friend class BufferCacheRuntime;
public:
    explicit Buffer(BufferCacheRuntime&, VideoCommon::NullBufferParams null_params);
    explicit Buffer(BufferCacheRuntime& runtime, VAddr cpu_addr_, u64 size_bytes_);

    [[nodiscard]] VkBufferView View(u32 offset, u32 size, VideoCore::Surface::PixelFormat format);

    /// View whose element count is the size the guest bound, the second view of a texel buffer
    /// descriptor when the device needs it (Device::NeedsTexelBufferPad)
    [[nodiscard]] VkBufferView SizeView(u32 offset, u32 size,
                                        VideoCore::Surface::PixelFormat format);

    [[nodiscard]] VkBuffer Handle() const noexcept {
        return *buffer;
    }

    [[nodiscard]] bool IsRegionUsed(u64 offset, u64 size) const noexcept {
        return tracker.IsUsed(offset, size);
    }

    void MarkUsage(u64 offset, u64 size) noexcept {
        tracker.Track(offset, size);
    }

    void ResetUsageTracking() noexcept {
        tracker.Reset();
    }

    operator VkBuffer() const noexcept {
        return *buffer;
    }

private:
    struct BufferView {
        u32 offset;
        u32 size;
        VideoCore::Surface::PixelFormat format;
        vk::BufferView handle;
        vk::BufferView size_handle;
    };

    [[nodiscard]] const BufferView* FindOrCreateView(u32 offset, u32 size,
                                                     VideoCore::Surface::PixelFormat format);

    const Device* device{};
    vk::Buffer buffer;
    std::vector<BufferView> views;
    VideoCommon::UsageTracker tracker;
    bool is_null{};
    std::shared_ptr<ReadbackShadow> readback_shadow;
};

class QuadArrayIndexBuffer;
class QuadStripIndexBuffer;

class BufferCacheRuntime {
    friend Buffer;

    using PrimitiveTopology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

public:
    explicit BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_manager_,
                                Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                                GuestDescriptorQueue& guest_descriptor_queue,
                                ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                                DescriptorPool& descriptor_pool);

    void TickFrame(Common::SlotVector<Buffer>& slot_buffers) noexcept;

    /// Notes that the commands being recorded write to a buffer.
    void MarkHostWrite(const Buffer& buffer);

    void Finish();

    u64 GetDeviceLocalMemory() const;

    u64 GetDeviceMemoryUsage() const;

    bool CanReportMemoryUsage() const;

    bool ShouldStreamUniformBuffer(const Buffer& buffer, u32 offset, u32 size) const;

    u32 GetStorageBufferAlignment() const;

    [[nodiscard]] StagingBufferRef UploadStagingBuffer(size_t size);

    [[nodiscard]] StagingBufferRef DownloadStagingBuffer(size_t size, bool deferred = false);

    bool CanReorderUpload(const Buffer& buffer, std::span<const VideoCommon::BufferCopy> copies);

    void FreeDeferredStagingBuffer(StagingBufferRef& ref);

    void PreCopyBarrier();

    void CopyBuffer(VkBuffer src_buffer, VkBuffer dst_buffer,
                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                    bool can_reorder_upload = false);

    void PostCopyBarrier();

    bool CanDifferentialReadback(const Buffer& buffer, const VideoCommon::BufferCopy& copy) const;
    bool DifferentialReadback(Buffer& buffer, const StagingBufferRef& data,
                              const StagingBufferRef& flags, const VideoCommon::BufferCopy& copy,
                              u64 flag_offset);
    bool HasReadbackReferences() const noexcept { return has_readback_references; }
    void InvalidateReadbackReference(Buffer& buffer, u64 offset, u64 size);
    std::shared_ptr<Common::ReadbackCpuReference> GetReadbackCpuReference(const Buffer& buffer) const;

    void ClearBuffer(VkBuffer dest_buffer, u32 offset, size_t size, u32 value);

    void BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format, u32 num_indices,
                         u32 base_vertex, VkBuffer buffer, u32 offset, u32 size);

    void BindQuadIndexBuffer(PrimitiveTopology topology, u32 first, u32 count);

    void BindVertexBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size, u32 stride);

    void BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings);

    bool SetVertexBindingMap(const std::array<u32, 32>& bindings) {
        if (vertex_binding_map == bindings) {
            return false;
        }
        vertex_binding_map = bindings;
        compact_vertex_bindings = bindings != VertexInputMapping{}.bindings;
        return true;
    }

    void BindTransformFeedbackBuffer(u32 index, VkBuffer buffer, u32 offset, u32 size);

    void BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings);

    /// Transform feedback buffer as bound by the guest, written by shaders on hosts without
    /// transform feedback
    struct EmulatedTransformFeedbackBuffer {
        VkBuffer buffer{};
        u32 offset{};
        u32 size{};
        /// Bytes from the offset to the end of the host buffer
        u64 capacity{};
        /// Bytes the draws captured since transform feedback started on this buffer
        u32 written{};
    };

    [[nodiscard]] std::array<EmulatedTransformFeedbackBuffer, 4>& EmulatedTransformFeedbackBuffers() {
        return emulated_xfb_buffers;
    }

    /// Tracks whether the draws capture. Capture starts from the beginning of the buffers again
    /// when it resumes, while the byte counts stay readable after it stops.
    void SetEmulatedTransformFeedbackEnabled(bool enabled) {
        if (enabled && !emulated_xfb_enabled) {
            for (EmulatedTransformFeedbackBuffer& buffer : emulated_xfb_buffers) {
                buffer.written = 0;
            }
        }
        emulated_xfb_enabled = enabled;
    }

    std::span<u8> BindMappedUniformBuffer([[maybe_unused]] size_t stage,
                                          [[maybe_unused]] u32 binding_index, u32 size) {
        const StagingBufferRef ref = staging_pool.Request(size, MemoryUsage::Upload);
        BindBuffer(ref.buffer, static_cast<u32>(ref.offset), size);
        return ref.mapped_span;
    }

    void BindUniformBuffer(VkBuffer buffer, u32 offset, u32 size) {
        BindBuffer(buffer, offset, size);
    }

    void BindStorageBuffer(VkBuffer buffer, u32 offset, u32 size,
                           [[maybe_unused]] bool is_written) {
        BindBuffer(buffer, offset, size);
    }

    void BindTextureBuffer(Buffer& buffer, u32 offset, u32 size,
                           VideoCore::Surface::PixelFormat format) {
        guest_descriptor_queue.AddTexelBuffer(buffer.View(offset, size, format));
        if (device.NeedsTexelBufferPad()) {
            guest_descriptor_queue.AddTexelBuffer(buffer.SizeView(offset, size, format));
        }
    }

private:
    void BindBuffer(VkBuffer buffer, u32 offset, u32 size) {
        guest_descriptor_queue.AddBuffer(buffer, offset, size);
    }

    void ReserveNullBuffer();
    vk::Buffer CreateNullBuffer();

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;
    GuestDescriptorQueue& guest_descriptor_queue;

    std::shared_ptr<QuadArrayIndexBuffer> quad_array_index_buffer;
    std::shared_ptr<QuadStripIndexBuffer> quad_strip_index_buffer;

    vk::Buffer null_buffer;
    std::array<u32, 32> vertex_binding_map = VertexInputMapping{}.bindings;
    bool compact_vertex_bindings{};
    std::array<EmulatedTransformFeedbackBuffer, 4> emulated_xfb_buffers{};
    bool emulated_xfb_enabled{};

    std::unique_ptr<Uint8Pass> uint8_pass;
    QuadIndexedPass quad_index_pass;
    std::unique_ptr<ReadbackDeltaPass> readback_delta_pass;
    std::shared_ptr<ReadbackShadowBudget> readback_shadow_budget;
    ComputePassDescriptorQueue& readback_descriptor_queue;
    DescriptorPool& readback_descriptor_pool;
    bool differential_readback_enabled{true};
    bool has_readback_references{};
};

struct BufferCacheParams {
    using Runtime = Vulkan::BufferCacheRuntime;
    using Buffer = Vulkan::Buffer;
    using Async_Buffer = Vulkan::StagingBufferRef;
    using MemoryTracker = VideoCommon::MemoryTrackerBase<Tegra::MaxwellDeviceMemoryManager>;

    static constexpr bool IS_OPENGL = false;
    static constexpr bool HAS_PERSISTENT_UNIFORM_BUFFER_BINDINGS = false;
    static constexpr bool HAS_FULL_INDEX_AND_PRIMITIVE_SUPPORT = false;
    static constexpr bool NEEDS_BIND_UNIFORM_INDEX = false;
    static constexpr bool NEEDS_BIND_STORAGE_INDEX = false;
    static constexpr bool USE_MEMORY_MAPS = true;
    static constexpr bool SEPARATE_IMAGE_BUFFER_BINDINGS = false;
    static constexpr bool USE_MEMORY_MAPS_FOR_UPLOADS = true;
};

using BufferCache = VideoCommon::BufferCache<BufferCacheParams>;
} // namespace Vulkan
