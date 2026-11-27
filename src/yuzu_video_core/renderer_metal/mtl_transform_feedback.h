// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_shader_recompiler/runtime_info.h"
#include "yuzu_shader_recompiler/vtg_as_compute.h"
#include "yuzu_video_core/transform_feedback.h"
#include "yuzu_video_core/metal_common/metal_memory_allocator.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/renderer_metal/mtl_staging_buffer_pool.h"

namespace Metal {

class Device;
class Scheduler;

/// Where a captured stream is written, in guest terms.
struct TransformFeedbackBinding {
    MTL::Buffer* buffer{};
    u32 offset{};
    u32 size{};
};

/// Per-stream GPU transform feedback counter (matches MSL XfbGpuCounter).
struct XfbGpuCounter {
    u64 bytes{};
    u64 primitives{};
};
static_assert(sizeof(XfbGpuCounter) == 16);

// Transform feedback, as two compute passes. Metal has no stream-out stage, so
// the vertex program is re-emitted as a compute kernel (see
// TranslateVertexToCaptureKernel) that writes its whole stage-out struct to a
// scratch buffer, one entry per vertex. That struct uses SPIRV-Cross's layout
// instead of the guest's, so a second kernel moves each varying the guest asked
// for into the buffer, offset and stride it named.
//
// The captured layout is a packed array of float4 slots and SPIRV-Cross decides
// which slot holds which output, not the guest's attribute numbering, so the
// caller has to describe it through Gather()'s slot_of_generic and
// position_slot.
class TransformFeedback : Common::NonCopyable, Common::NonMoveable {
public:
    explicit TransformFeedback(const Device& device, Scheduler& scheduler,
                               MemoryAllocator& memory_allocator);
    ~TransformFeedback();

    [[nodiscard]] bool IsValid() const noexcept {
        return static_cast<bool>(gather_pipeline);
    }

    /// Scratch the capture kernel writes into, grown as needed. Null when the
    /// requested size cannot be allocated.
    [[nodiscard]] MTL::Buffer* CaptureBuffer(u32 bytes);

    /// A standalone capture buffer the caller owns, for captures whose data has
    /// to survive until a deferred drain, where a shared growing buffer would
    /// lose earlier segments on reallocation.
    [[nodiscard]] Owned<MTL::Buffer> CreateCaptureSegment(u32 bytes);

    /// Borrows a capture buffer of at least @p bytes from the pool. Pooled
    /// because every buffer the memory allocator hands out joins the device's
    /// residency set, which retains it, and nothing on this path removes it
    /// again, so one buffer per capture draw leaked residency at gigabytes a
    /// minute. Reuse is guarded twice: a segment is not handed out again while a
    /// pending gather still has to read it on the CPU, nor before the GPU has
    /// passed the tick its capture was recorded in. Returns null if nothing could
    /// be allocated. The pool owns the buffer, the caller borrows it until it
    /// calls ReleaseCaptureSegment.
    [[nodiscard]] MTL::Buffer* AcquireCaptureSegment(u32 bytes);

    /// Hands a borrowed segment back. Safe to call with null or with a pointer
    /// the pool does not own. The segment is not reusable until the frame ends
    /// and StampReleasedSegments names the tick it retires after.
    void ReleaseCaptureSegment(MTL::Buffer* segment);

    /// Records which submission every segment released this frame has to
    /// outlive. Call once at frame end, after the frame's last draw is recorded.
    /// Captured vertices feed draws recorded after the capture, which can land in
    /// any later submission of the same frame, so the producing submission
    /// completing says nothing about when the segment is free. Frame end is the
    /// first moment we know an upper bound, nothing further gets recorded
    /// against it.
    void StampReleasedSegments();

    /// Bytes one captured vertex occupies, for @p num_outputs vec4 outputs.
    [[nodiscard]] static constexpr u32 CaptureStride(u32 num_outputs) noexcept {
        return num_outputs * 16;
    }

    /// Records the gather: capture buffer in, guest buffers out.
    /// @param varyings        Indexed by attribute word, as
    ///                        MakeTransformFeedbackVaryings returns.
    /// @param num_vertices    Entries written by the capture kernel.
    /// @param slot_of_generic Capture float4 slot per generic location, -1 when
    ///                        the vertex program does not store it. SPIRV-Cross
    ///                        packs stored generics ascending. The caller knows
    ///                        which are stored, this class does not.
    /// @param position_slot   Capture float4 slot of gl_Position (last).
    void Gather(MTL::Buffer* capture, u32 capture_stride, u32 num_vertices,
                std::span<const Shader::TransformFeedbackVarying> varyings,
                std::span<const TransformFeedbackBinding> bindings,
                std::span<const s8> slot_of_generic, s32 position_slot);

    /// 100% GPU compaction + gather for combined Geometry Shader + Transform Feedback.
    /// Reads emitted vertices and indices produced by VtgGeometryToCompute, computes exact
    /// per-primitive destination offsets via GPU prefix sum, scatters each varying starting
    /// at counter_buffer[buffer_index].bytes, and advances counter_buffer[stream] on the GPU
    /// without any CPU readback or flush between consecutive draws.
    [[nodiscard]] bool GatherVtg(StagingBufferPool& staging_pool,
                                 const StagingBufferRef& geometry_out, u32 geometry_stride_bytes,
                                 u32 max_output_vertices, const StagingBufferRef& index_out,
                                 u32 num_primitives, u32 indices_per_primitive, u32 sentinel,
                                 Shader::OutputTopology output_topology,
                                 const Shader::VtgVaryingLayout& geometry_layout,
                                 const VideoCommon::TransformFeedbackState& xfb_state,
                                 std::span<const TransformFeedbackBinding> bindings,
                                 MTL::Buffer* counter_buffer = nullptr,
                                 u32 active_counter_mask = 0);

private:
    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;

    Owned<MTL::Library> library;
    Owned<MTL::ComputePipelineState> gather_pipeline;
    Owned<MTL::ComputePipelineState> vtg_count_scan_pipeline;
    Owned<MTL::ComputePipelineState> vtg_scan_groups_pipeline;
    Owned<MTL::ComputePipelineState> vtg_scatter_pipeline;
    Owned<MTL::ComputePipelineState> vtg_advance_counters_pipeline;

    Owned<MTL::Buffer> capture_buffer;
    u32 capture_capacity{};
    size_t logged_pool_high_water{};

    /// One reusable capture buffer. `held` covers the CPU read the frame-end
    /// drain still owes, `free_after` covers the GPU work already recorded
    /// against it.
    struct PooledSegment {
        Owned<MTL::Buffer> buffer;
        u32 capacity{};
        bool held{};
        /// UNSTAMPED until the frame ends; see StampReleasedSegments.
        u64 free_after{UNSTAMPED};
    };

    /// A released segment whose retirement tick is not known yet. Never free,
    /// because no tick compares greater than this.
    static constexpr u64 UNSTAMPED = ~u64{0};

    std::vector<PooledSegment> segment_pool;
};

} // namespace Metal
