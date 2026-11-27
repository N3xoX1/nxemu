// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"

namespace Metal {

class Device;
class Scheduler;
class StagingBufferPool;


/// Where a rewritten index buffer ended up. When `valid` is false the pass could
/// not run and the draw has to be dropped, the guest's own indices would draw
/// the wrong primitives.
struct ConvertedIndices {
    MTL::Buffer* buffer{};
    u32 offset{};
    u32 num_indices{};
    bool valid{};
};

/// Indirect draw arguments rewritten so entries past the GPU-written count draw
/// nothing. `valid` is false when the patch could not run, in which case the
/// caller must fall back to a CPU-read count.
struct PatchedIndirect {
    MTL::Buffer* buffer{};
    u32 offset{};
    u32 stride{};
    bool valid{};
};

/// Parameters for GPU-side conditional rendering (mirrors MSL ConditionalDrawParams).
struct ConditionalDrawParams {
    u64 a_cpu_value{};
    u64 b_cpu_value{};
    u32 mode{};          ///< 2 = Conditional, 3 = IfEqual, 4 = IfNotEqual
    u32 flags{};         ///< bit0:a_gpu bit1:a_64 bit2:b_gpu bit3:b_64 bit4:indexed
    u32 a_first_slot{};
    u32 a_slot_count{};
    u32 b_first_slot{};
    u32 b_slot_count{};
    u32 vis_mask{};
    u32 draw_count{};
    u32 num_instances{};
    u32 base_vertex{};
    u32 base_instance{};
    u32 reserved{};
};
static_assert(sizeof(ConditionalDrawParams) == 64);

// Rewrites guest index buffers into something Metal can draw, on the GPU. Metal
// has no fan, quad, quad-strip, polygon or line-loop primitive and no 8-bit
// index type. The guest's indices live in GPU memory with writes possibly still
// in flight, so we cannot rewrite them on the CPU without racing.
//
// Output is always 32-bit indices with the base vertex folded in, so the draw
// itself uses a base vertex of zero.
class IndexPass : Common::NonCopyable, Common::NonMoveable {
public:
    explicit IndexPass(const Device& device, Scheduler& scheduler,
                       StagingBufferPool& staging_pool);
    ~IndexPass();

    /// Expands an indexed draw whose topology Metal cannot draw.
    /// @param num_indices Source indices the guest supplied.
    [[nodiscard]] ConvertedIndices Convert(
        Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology,
        Tegra::Engines::Maxwell3D::Regs::IndexFormat format, u32 num_indices, u32 base_vertex,
        MTL::Buffer* src, u32 src_offset);

    /// Widens an index buffer whose format Metal has no type for, and remaps a
    /// restart index onto the one Metal recognises.
    ///
    /// @param restart_index Index the guest treats as a strip restart. Pass
    ///        0xFFFFFFFF when there is nothing to remap.
    /// @param list_verts_per_prim Vertices per primitive when the topology is a
    ///        LIST, which Metal never restarts on. Non-zero makes the pass
    ///        collapse each primitive containing a marker onto vertex 0 instead
    ///        of remapping the marker, since a marker left in a list buffer is
    ///        fetched as a real, wildly out of bounds, vertex. Zero for strips
    ///        and fans, where Metal's own restart does the work.
    [[nodiscard]] ConvertedIndices Widen(Tegra::Engines::Maxwell3D::Regs::IndexFormat format,
                                         u32 num_indices, u32 base_vertex, MTL::Buffer* src,
                                         u32 src_offset, u32 restart_index = 0xFFFFFFFFu,
                                         u32 list_verts_per_prim = 0);

    /// Flattens a draw's primitives into one 32-bit index per primitive vertex,
    /// so `primitive * verts_per_prim + k` addresses vertex k of primitive p.
    /// Feeds geometry shader emulation, whose lowered kernel reads a captured
    /// buffer at exactly that stride. Runs for topologies Metal draws natively
    /// too, what matters is the order the *capture* visits vertices in and not
    /// what Metal can rasterise.
    /// @param is_indexed When false @p src is ignored and the indices are the
    ///        vertex numbers themselves, starting at @p base_vertex.
    /// @param count Indices (or vertices) the guest supplied.
    /// @return num_indices is `primitives * verts_per_prim`, invalid when the
    ///         topology has no expansion here.
    [[nodiscard]] ConvertedIndices ExpandPrimitives(
        Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology,
        Tegra::Engines::Maxwell3D::Regs::IndexFormat format, bool is_indexed, u32 count,
        u32 base_vertex, MTL::Buffer* src, u32 src_offset);

    /// Vertices one primitive of @p topology is assembled from, or 0 when
    /// ExpandPrimitives() cannot express it.
    [[nodiscard]] static u32 VerticesPerPrimitive(
        Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology);

    /// Primitives @p count indices of @p topology assemble into.
    [[nodiscard]] static u32 PrimitiveCount(
        Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology, u32 count);

    /// Converts a transform-feedback byte count into indirect draw arguments,
    /// Vulkan's DrawIndirectByteCountEXT which Metal has no equivalent of. The
    /// count is GPU-written, so a one-thread kernel divides it by the vertex
    /// stride and writes MTLDrawPrimitivesIndirectArguments. Reading it on the
    /// CPU would be a frame stale.
    [[nodiscard]] PatchedIndirect MakeByteCountDrawArgs(MTL::Buffer* count_buffer,
                                                        u32 count_offset, u32 stride);

    /// Emulates DrawIndirectCount by rewriting the argument entries. Metal
    /// encodes one command per indirect entry and cannot take a count buffer, so
    /// every allowed entry is encoded and the surplus ones are zeroed to draw
    /// nothing. Copied into scratch instead of patched in place, so the guest's
    /// buffer survives for later frames.
    [[nodiscard]] PatchedIndirect PatchIndirectCount(MTL::Buffer* src, u32 src_offset, u32 stride,
                                                     u32 max_entries, MTL::Buffer* count_buffer,
                                                     u32 count_offset, bool is_indexed,
                                                     bool use_upload_compute = false);

    /// Writes @p value into every 32-bit word of a buffer range. A blit encoder
    /// can only fill a *byte* pattern, so a value whose four bytes differ cannot
    /// be expressed that way.
    void Fill(MTL::Buffer* dst, u32 offset, u32 num_words, u32 value);

    /// Prepares a single non-indexed indirect draw whose vertex and geometry
    /// stages run as compute (mirrors vulkan_vtg_indirect.comp). Returns a
    /// buffer holding 11 u32 words: vertex dispatch (0..2), geometry dispatch
    /// (3..5), and MTLDrawIndexedPrimitivesIndirectArguments (6..10).
    [[nodiscard]] PatchedIndirect PrepareVtgIndirect(MTL::Buffer* args_buffer, u32 args_offset,
                                                     MTL::Buffer* info_buffer, u32 info_offset,
                                                     MTL::Buffer* feedback_buffer,
                                                     u32 feedback_offset, u32 topology,
                                                     u32 capacity, u32 indices_per_primitive,
                                                     u32 instance_mask);

    [[nodiscard]] bool HasVtgIndirect() const noexcept {
        return static_cast<bool>(vtg_indirect_state);
    }

    /// Evaluates a conditional rendering predicate (from occlusion visibility slots
    /// or directly from a GPU-modified BufferCache buffer when `flags & 32`) on the
    /// GPU (on Metal 4's upload command buffer so the active TBDR render pass stays
    /// open!) and writes MTLDraw{Indexed}PrimitivesIndirectArguments with
    /// instanceCount = 0 when the predicate is false. Caller must ensure the
    /// referenced buffer was written in a strictly earlier submission.
    [[nodiscard]] PatchedIndirect MakeConditionalDrawArgs(MTL::Buffer* visibility_buffer,
                                                          const ConditionalDrawParams& params,
                                                          u32 src_offset = 0);

    [[nodiscard]] bool HasConditionalDraw() const noexcept {
        return static_cast<bool>(conditional_draw_state);
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return static_cast<bool>(pipeline_state);
    }

private:
    /// Kernel mode. Must match the switch in the MSL.
    enum class Mode : u32 {
        Quads = 0,
        QuadStrip = 1,
        Fan = 2,
        LineLoop = 3,
        Widen = 4,
        Fill = 5,
        Expand = 6,
    };

    /// How ExpandPrimitives() walks the source for one primitive. Must match the
    /// switch in the MSL.
    enum class ExpandMode : u32 {
        /// Independent primitives: vertex k of primitive p is at p * V + k.
        List = 0,
        /// A strip: consecutive primitives overlap all but one vertex.
        Strip = 1,
        /// A fan: every primitive shares vertex 0.
        Fan = 2,
        /// A triangle strip, whose odd primitives swap their first two vertices
        /// so the winding survives being expanded into independent triangles.
        TriangleStrip = 3,
    };

    [[nodiscard]] ConvertedIndices Run(Mode mode, u32 index_size, u32 num_source, u32 num_output,
                                       u32 num_threads, u32 base_vertex, MTL::Buffer* src,
                                       u32 src_offset, u32 value = 0xFFFFFFFFu,
                                       u32 verts_per_prim = 0, u32 expand_mode = 0);

    const Device& device;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;

    Owned<MTL::Library> library;
    Owned<MTL::ComputePipelineState> pipeline_state;
    /// See PatchIndirectCount.
    Owned<MTL::ComputePipelineState> indirect_patch_state;
    /// See MakeByteCountDrawArgs.
    Owned<MTL::ComputePipelineState> byte_count_state;
    /// See PrepareVtgIndirect.
    Owned<MTL::ComputePipelineState> vtg_indirect_state;
    /// See MakeConditionalDrawArgs.
    Owned<MTL::ComputePipelineState> conditional_draw_state;

    static constexpr u32 CONDITIONAL_ARENA_SIZE = 64 * 1024;
    static constexpr u32 CONDITIONAL_ENTRY_STRIDE = 32;
    static constexpr u32 INDIRECT_ARENA_SIZE = 64 * 1024;
    struct ScratchArena {
        MTL::Buffer* buffer{};
        u32 base{};
        u32 used{};
        u32 capacity{};
        u64 tick{};
    };
    ScratchArena conditional_arena;
    ScratchArena indirect_arena;
};

} // namespace Metal
