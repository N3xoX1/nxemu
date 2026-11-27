// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "yuzu_common/div_ceil.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_index_pass.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_video_core/renderer_metal/mtl_shader_util.h"
#include "yuzu_video_core/renderer_metal/mtl_staging_buffer_pool.h"

namespace Metal {


namespace {

using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
using IndexFormat = Tegra::Engines::Maxwell3D::Regs::IndexFormat;

constexpr u32 THREADGROUP_SIZE = 64;

constexpr u32 SOURCE_INDEX = 0;
constexpr u32 DESTINATION_INDEX = 1;
constexpr u32 PARAMS_INDEX = 2;

/// Mirrors `Params` in the MSL. Kept plain so setBytes can push it inline.
struct Params {
    u32 index_size;
    u32 base_vertex;
    u32 num_source;
    u32 num_threads;
    u32 mode;
    u32 value;
    u32 verts_per_prim;
    u32 expand_mode;
};

/// Source indices are read byte by byte: the guest picks the index width and the
/// buffer offset, so neither is guaranteed to be aligned for a typed `ushort` or
/// `uint` load, which is undefined in MSL.
constexpr const char* CONVERT_MSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Params {
    uint index_size;
    uint base_vertex;
    uint num_source;
    uint num_threads;
    uint mode;
    uint value;
    uint verts_per_prim;
    uint expand_mode;
};

static uint ReadIndex(device const uchar* src, uint at, uint size) {
    if (size == 1) {
        return uint(src[at]);
    }
    if (size == 2) {
        const uint base = at * 2;
        return uint(src[base]) | (uint(src[base + 1]) << 8);
    }
    const uint base = at * 4;
    return uint(src[base]) | (uint(src[base + 1]) << 8) |
           (uint(src[base + 2]) << 16) | (uint(src[base + 3]) << 24);
}

// Emulates DrawIndirectCount: every allowed entry is copied into scratch, and
// instanceCount is zeroed on entries at or past the GPU-written count so they
// draw nothing. instanceCount is the second u32 of both
// MTLDrawPrimitivesIndirectArguments and its indexed counterpart, so one kernel
// covers both.
struct IndirectPatch {
    uint max_entries;
    uint src_stride_words;
    uint dst_stride_words;
    uint words_per_entry;
    uint count_word;
};

// Turns a transform-feedback BYTE count into draw arguments, Vulkan's
// DrawIndirectByteCountEXT which Metal lacks. The count is GPU-written, so we
// divide it by the vertex stride here instead of on the CPU.
kernel void byte_count_draw_args(device const uint* count_buffer [[buffer(0)]],
                                 device uint* dst [[buffer(1)]],
                                 constant uint2& info [[buffer(2)]],
                                 uint id [[thread_position_in_grid]]) {
    if (id != 0u) {
        return;
    }
    const uint bytes = count_buffer[info.x];
    dst[0] = info.y != 0u ? (bytes / info.y) : 0u; // vertexCount
    dst[1] = 1u;                                   // instanceCount
    dst[2] = 0u;                                   // vertexStart
    dst[3] = 0u;                                   // baseInstance
}

kernel void patch_indirect(device const uint* src [[buffer(0)]],
                           device const uint* count_buffer [[buffer(1)]],
                           device uint* dst [[buffer(2)]],
                           constant IndirectPatch& info [[buffer(3)]],
                           uint entry [[thread_position_in_grid]]) {
    if (entry >= info.max_entries) {
        return;
    }
    const uint count = count_buffer[info.count_word];
    const uint src_base = entry * info.src_stride_words;
    const uint dst_base = entry * info.dst_stride_words;
    for (uint word = 0; word < info.words_per_entry; ++word) {
        dst[dst_base + word] = src[src_base + word];
    }
    if (entry >= count) {
        dst[dst_base + 1] = 0u; // instanceCount
    }
}

kernel void convert_indices(device const uchar* src [[buffer(0)]],
                            device uint* dst [[buffer(1)]],
                            constant Params& p [[buffer(2)]],
                            uint id [[thread_position_in_grid]]) {
    if (id >= p.num_threads) {
        return;
    }

    // Fill: write a 32-bit pattern, which a blit encoder's byte fill cannot do.
    if (p.mode == 5u) {
        dst[id] = p.value;
        return;
    }

    // Primitive expansion: one thread per primitive, verts_per_prim indices out.
    // index_size == 0 means the draw was not indexed, so a vertex's "index" is
    // its own number.
    if (p.mode == 6u) {
        const uint verts = p.verts_per_prim;
        for (uint k = 0u; k < verts; ++k) {
            uint at;
            if (p.expand_mode == 0u) {
                at = id * verts + k;
            } else if (p.expand_mode == 2u) {
                at = (k == 0u) ? 0u : (id + k);
            } else if (p.expand_mode == 3u) {
                // Odd triangles of a strip swap their first two vertices.
                const uint swapped = ((id & 1u) != 0u && k < 2u) ? (1u - k) : k;
                at = id + swapped;
            } else {
                at = id + k;
            }
            if (at >= p.num_source) {
                at = p.num_source - 1u;
            }
            const uint index = (p.index_size == 0u) ? at : ReadIndex(src, at, p.index_size);
            dst[id * verts + k] = index + p.base_vertex;
        }
        return;
    }

    // Widen: one source index in, one 32-bit index out. `value` is the guest's
    // restart index (0xFFFFFFFF when there is none). Metal only restarts at the
    // index type's maximum, so the guest's marker is remapped onto it, without
    // base_vertex since it is a marker and not a vertex.
    //
    // verts_per_prim is non-zero only for a LIST topology, where Metal has no
    // restart at all and a marker left in the buffer is fetched as a real vertex.
    // 0xFFFF against a stride of 36 reads 2.3 MB past the stream and faults the
    // GPU. The guest's hardware discards the primitive containing the marker, so
    // collapse all of its indices onto vertex 0. Zero area so nothing rasterises,
    // and vertex 0 exists in every bound stream.
    if (p.mode == 4u) {
        const uint index = ReadIndex(src, id, p.index_size);
        if (p.verts_per_prim > 1u) {
            const uint first = (id / p.verts_per_prim) * p.verts_per_prim;
            for (uint k = 0u; k < p.verts_per_prim; ++k) {
                const uint at = first + k;
                if (at < p.num_source && ReadIndex(src, at, p.index_size) == p.value) {
                    dst[id] = 0u;
                    return;
                }
            }
            dst[id] = index + p.base_vertex;
            return;
        }
        dst[id] = index == p.value ? 0xFFFFFFFFu : index + p.base_vertex;
        return;
    }

    // Line loop: the strip, then the closing segment back to the first vertex.
    if (p.mode == 3u) {
        const uint at = (id < p.num_source) ? id : 0u;
        dst[id] = ReadIndex(src, at, p.index_size) + p.base_vertex;
        return;
    }

    // Fan and polygon: every triangle shares the first vertex.
    if (p.mode == 2u) {
        dst[id * 3u + 0u] = ReadIndex(src, 0u, p.index_size) + p.base_vertex;
        dst[id * 3u + 1u] = ReadIndex(src, id + 1u, p.index_size) + p.base_vertex;
        dst[id * 3u + 2u] = ReadIndex(src, id + 2u, p.index_size) + p.base_vertex;
        return;
    }

    // Quads and quad strips. The corner order matches the Vulkan backend's
    // generated buffers. Reverse it and the facing flips and the quad is culled.
    uint corners[6];
    uint stride;
    if (p.mode == 0u) {
        stride = 4u;
        corners[0] = 0u; corners[1] = 1u; corners[2] = 2u;
        corners[3] = 0u; corners[4] = 2u; corners[5] = 3u;
    } else {
        stride = 2u;
        corners[0] = 0u; corners[1] = 3u; corners[2] = 1u;
        corners[3] = 0u; corners[4] = 2u; corners[5] = 3u;
    }

    const uint base = id * stride;
    for (uint k = 0u; k < 6u; ++k) {
        dst[id * 6u + k] = ReadIndex(src, base + corners[k], p.index_size) + p.base_vertex;
    }
}

struct VtgIndirectParams {
    uint args_word;
    uint topology;
    uint capacity;
    uint indices_per_primitive;
    uint instance_mask;
};

static uint VtgPrimitiveCountMsl(uint topology, uint count) {
    switch (topology) {
    case 0u: return count;                              // Points
    case 1u: return count / 2u;                         // Lines
    case 2u: return count >= 2u ? count : 0u;           // LineLoop
    case 3u: return count >= 2u ? count - 1u : 0u;      // LineStrip
    case 4u: return count / 3u;                         // Triangles
    case 5u:                                            // TriangleStrip
    case 6u: return count >= 3u ? count - 2u : 0u;      // TriangleFan
    case 7u: return (count / 4u) * 2u;                  // Quads
    case 8u: return count >= 4u ? ((count - 2u) / 2u) * 2u : 0u; // QuadStrip
    case 9u: return count / 4u;                         // LinesAdjacency
    case 10u: return count >= 4u ? count - 3u : 0u;     // LineStripAdjacency
    case 11u: return count / 6u;                        // TrianglesAdjacency
    case 12u: return count >= 6u ? (count - 4u) / 2u : 0u; // TriangleStripAdjacency
    default: return 0u;
    }
}

kernel void prepare_vtg_indirect(device const uint* args [[buffer(0)]],
                                 device uint* info [[buffer(1)]],
                                 device uint* commands [[buffer(2)]],
                                 device atomic_uint* feedback [[buffer(3)]],
                                 constant VtgIndirectParams& p [[buffer(4)]]) {
    constexpr uint LOCAL_SIZE = 32u;
    const uint count = args[p.args_word];
    const uint instances = args[p.args_word + 1u] & p.instance_mask;
    const uint first = args[p.args_word + 2u];
    const uint base_instance = args[p.args_word + 3u];

    const uint per_instance = VtgPrimitiveCountMsl(p.topology, count);
    const bool overflows = count != 0u && instances > (0xffffffffu / count);
    atomic_fetch_max_explicit(&feedback[0], overflows ? 0xffffffffu : (count * instances),
                              memory_order_relaxed);

    const uint drawn = count == 0u ? 0u : min(instances, p.capacity / count);
    if (count != 0u && drawn < instances) {
        atomic_store_explicit(&feedback[1], 1u, memory_order_relaxed);
    }
    const uint vertices = count * drawn;
    const uint primitives = per_instance * drawn;

    info[0] = count;
    info[1] = max(drawn, 1u);
    info[2] = first;
    info[3] = base_instance;
    info[6] = per_instance;

    // Vertex compute dispatch indirect
    commands[0] = primitives == 0u ? 0u : (vertices + LOCAL_SIZE - 1u) / LOCAL_SIZE;
    commands[1] = 1u;
    commands[2] = 1u;
    // Geometry compute dispatch indirect
    commands[3] = (primitives + LOCAL_SIZE - 1u) / LOCAL_SIZE;
    commands[4] = 1u;
    commands[5] = 1u;
    // MTLDrawIndexedPrimitivesIndirectArguments
    commands[6] = primitives * p.indices_per_primitive;
    commands[7] = 1u;
    commands[8] = 0u;
    commands[9] = 0u;
    commands[10] = 0u;
}

struct ConditionalDrawParams {
    ulong a_cpu_value;
    ulong b_cpu_value;
    uint mode;
    uint flags;
    uint a_first_slot;
    uint a_slot_count;
    uint b_first_slot;
    uint b_slot_count;
    uint vis_mask;
    uint draw_count;
    uint num_instances;
    uint base_vertex;
    uint base_instance;
    uint reserved;
};

static ulong SumVisibilitySlots(device const ulong* vis, uint first_slot, uint count, uint mask) {
    ulong total = 0ul;
    for (uint i = 0u; i < count; ++i) {
        total += vis[(first_slot + i) & mask];
    }
    return total;
}

kernel void conditional_draw_args(device const ulong* vis [[buffer(0)]],
                                  device uint* dst [[buffer(1)]],
                                  constant ConditionalDrawParams& p [[buffer(2)]]) {
    bool execute = true;
    if ((p.flags & 32u) != 0u) {
        // BufferCache predicate mode: buffer(0) is bound directly at Maxwell3D's
        // ReportSemaphore::Compare struct (initial_sequence, initial_mode, pad0, pad1,
        // current_sequence, current_mode).
        device const uint* cmp = reinterpret_cast<device const uint*>(vis);
        if (p.mode == 2u) {
            execute = cmp[0] != 0u && cmp[1] != 0u;
        } else if (p.mode == 3u) {
            execute = cmp[0] == cmp[4] && cmp[1] == cmp[5];
        } else if (p.mode == 4u) {
            execute = cmp[0] != cmp[4] || cmp[1] != cmp[5];
        }
    } else {
        ulong val_a = p.a_cpu_value;
        if ((p.flags & 1u) != 0u) {
            const ulong sum_a =
                SumVisibilitySlots(vis, p.a_first_slot, p.a_slot_count, p.vis_mask);
            val_a = (p.flags & 2u) != 0u
                        ? sum_a
                        : ((p.a_cpu_value & 0xFFFFFFFF00000000ul) | (sum_a & 0xFFFFFFFFul));
        }
        if (p.mode == 2u) {
            // RenderEnable::Mode::Conditional (matches Maxwell3D::ProcessQueryCondition)
            execute = (val_a & 0xFFFFFFFFul) != 0ul && (val_a >> 32u) != 0ul;
        } else {
            ulong val_b = p.b_cpu_value;
            if ((p.flags & 4u) != 0u) {
                const ulong sum_b =
                    SumVisibilitySlots(vis, p.b_first_slot, p.b_slot_count, p.vis_mask);
                val_b = (p.flags & 8u) != 0u
                            ? sum_b
                            : ((p.b_cpu_value & 0xFFFFFFFF00000000ul) | (sum_b & 0xFFFFFFFFul));
            }
            if (p.mode == 3u) {
                execute = (val_a == val_b);
            } else if (p.mode == 4u) {
                execute = (val_a != val_b);
            }
        }
    }
    const uint instances = execute ? p.num_instances : 0u;
    if ((p.flags & 16u) != 0u) {
        // MTLDrawIndexedPrimitivesIndirectArguments
        dst[0] = p.draw_count;
        dst[1] = instances;
        dst[2] = 0u; // indexStart (already folded into index buffer offset)
        dst[3] = p.base_vertex;
        dst[4] = p.base_instance;
    } else {
        // MTLDrawPrimitivesIndirectArguments
        dst[0] = p.draw_count;
        dst[1] = instances;
        dst[2] = p.base_vertex;
        dst[3] = p.base_instance;
    }
}
)MSL";

u32 IndexSize(IndexFormat format) {
    switch (format) {
    case IndexFormat::UnsignedByte:
        return 1;
    case IndexFormat::UnsignedShort:
        return 2;
    case IndexFormat::UnsignedInt:
        return 4;
    }
    return 0;
}

} // Anonymous namespace

IndexPass::IndexPass(const Device& device_, Scheduler& scheduler_,
                     StagingBufferPool& staging_pool_)
    : device{device_}, scheduler{scheduler_}, staging_pool{staging_pool_} {
    ScopedAutoreleasePool pool;

    NS::Error* error = nullptr;
    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
    library = Adopt(device.GetDevice()->newLibrary(ToNSString(CONVERT_MSL), options, &error));
    options->release();

    if (!library) {
        LOG_ERROR(Render_Metal, "Failed to compile the index conversion kernel: {}",
                  DescribeError(error));
        return;
    }
    library->setLabel(MTLSTR("eden index conversion"));

    pipeline_state = BuildComputePipeline(device, library.get(), "convert_indices",
                                          "eden index conversion");

    // Optional helpers: callers fall back to CPU paths when null.
    byte_count_state =
        BuildComputePipeline(device, library.get(), "byte_count_draw_args", "eden byte count");
    indirect_patch_state =
        BuildComputePipeline(device, library.get(), "patch_indirect", "eden indirect patch");
    vtg_indirect_state = BuildComputePipeline(device, library.get(), "prepare_vtg_indirect",
                                              "nxemu vtg indirect");
    conditional_draw_state = BuildComputePipeline(device, library.get(), "conditional_draw_args",
                                                  "nxemu conditional draw");
}

IndexPass::~IndexPass() = default;

PatchedIndirect IndexPass::MakeConditionalDrawArgs(MTL::Buffer* visibility_buffer,
                                                   const ConditionalDrawParams& params,
                                                   u32 src_offset) {
    if (!conditional_draw_state || visibility_buffer == nullptr) {
        return {};
    }
    const u64 current_tick = scheduler.CurrentTick();
    if (conditional_arena.buffer == nullptr || conditional_arena.tick != current_tick ||
        conditional_arena.used + CONDITIONAL_ENTRY_STRIDE > conditional_arena.capacity) {
        const StagingBufferRef chunk =
            staging_pool.Request(CONDITIONAL_ARENA_SIZE, MemoryUsage::DeviceLocal);
        if (chunk.buffer == nullptr) {
            return {};
        }
        conditional_arena = ScratchArena{
            .buffer = chunk.buffer,
            .base = static_cast<u32>(chunk.offset),
            .used = 0,
            .capacity = CONDITIONAL_ARENA_SIZE,
            .tick = current_tick,
        };
    }
    const u32 dst_offset = conditional_arena.base + conditional_arena.used;
    conditional_arena.used += CONDITIONAL_ENTRY_STRIDE;

    MTL::Buffer* const dst_buffer = conditional_arena.buffer;
    MTL::ComputePipelineState* const state = conditional_draw_state.get();
    scheduler.RecordUploadCompute([state, visibility_buffer, src_offset, dst_buffer, dst_offset,
                                   params](ComputeEncoder& encoder) {
        encoder->setComputePipelineState(state);
        encoder->setBuffer(visibility_buffer, src_offset, 0);
        encoder->setBuffer(dst_buffer, dst_offset, 1);
        encoder->setBytes(&params, sizeof(params), 2);
        encoder->dispatchThreads(MTL::Size::Make(1, 1, 1), MTL::Size::Make(1, 1, 1));
    });
    return PatchedIndirect{dst_buffer, dst_offset, CONDITIONAL_ENTRY_STRIDE, true};
}

PatchedIndirect IndexPass::MakeByteCountDrawArgs(MTL::Buffer* count_buffer, u32 count_offset,
                                                u32 stride) {
    if (!byte_count_state || count_buffer == nullptr || stride == 0) {
        return {};
    }
    constexpr u32 ARGS_BYTES = 4 * sizeof(u32);
    StagingBufferRef scratch = staging_pool.Request(ARGS_BYTES, MemoryUsage::DeviceLocal);
    if (scratch.buffer == nullptr) {
        return {};
    }
    MTL::ComputePipelineState* state = byte_count_state.get();
    const std::array<u32, 2> info{count_offset / static_cast<u32>(sizeof(u32)), stride};
    // Opening a compute encoder closes any render pass on the worker, so the
    // recording-side mirror must be told or the next draw is silently dropped.
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([state, count_buffer, scratch, info](CommandContext& context) {
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }
        encoder->setComputePipelineState(state);
        encoder->setBuffer(count_buffer, 0, 0);
        encoder->setBuffer(scratch.buffer, scratch.offset, 1);
        encoder->setBytes(info.data(), sizeof(info), 2);
        encoder->dispatchThreads(MTL::Size::Make(1, 1, 1), MTL::Size::Make(1, 1, 1));
        encoder->barrierBeforeQueueStages(
            static_cast<MTL::Stages>(MTL::StageDispatch | MTL::StageBlit), MTL::StageAll);
    });
    return PatchedIndirect{scratch.buffer, static_cast<u32>(scratch.offset), ARGS_BYTES, true};
}

PatchedIndirect IndexPass::PrepareVtgIndirect(MTL::Buffer* args_buffer, u32 args_offset,
                                              MTL::Buffer* info_buffer, u32 info_offset,
                                              MTL::Buffer* feedback_buffer, u32 feedback_offset,
                                              u32 topology, u32 capacity,
                                              u32 indices_per_primitive, u32 instance_mask) {
    if (!vtg_indirect_state || args_buffer == nullptr || info_buffer == nullptr ||
        feedback_buffer == nullptr) {
        return {};
    }
    constexpr u32 COMMAND_BYTES = 11 * sizeof(u32);
    StagingBufferRef commands = staging_pool.Request(COMMAND_BYTES, MemoryUsage::DeviceLocal);
    if (commands.buffer == nullptr) {
        return {};
    }
    const u32 aligned_args = Common::AlignDown(args_offset, 16u);
    const std::array<u32, 5> params{
        (args_offset - aligned_args) / static_cast<u32>(sizeof(u32)),
        topology,
        capacity,
        indices_per_primitive,
        instance_mask,
    };
    MTL::ComputePipelineState* const state = vtg_indirect_state.get();
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([state, args_buffer, aligned_args, info_buffer, info_offset, commands,
                      feedback_buffer, feedback_offset, params](CommandContext& context) {
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }
        encoder->setComputePipelineState(state);
        encoder->setBuffer(args_buffer, aligned_args, 0);
        encoder->setBuffer(info_buffer, info_offset, 1);
        encoder->setBuffer(commands.buffer, commands.offset, 2);
        encoder->setBuffer(feedback_buffer, feedback_offset, 3);
        encoder->setBytes(params.data(), sizeof(params), 4);
        encoder->dispatchThreads(MTL::Size::Make(1, 1, 1), MTL::Size::Make(1, 1, 1));
        encoder->barrierBeforeQueueStages(
            static_cast<MTL::Stages>(MTL::StageDispatch | MTL::StageBlit), MTL::StageAll);
    });
    return PatchedIndirect{commands.buffer, static_cast<u32>(commands.offset), COMMAND_BYTES, true};
}

PatchedIndirect IndexPass::PatchIndirectCount(MTL::Buffer* src, u32 src_offset, u32 stride,
                                             u32 max_entries, MTL::Buffer* count_buffer,
                                             u32 count_offset, bool is_indexed,
                                             bool use_upload_compute) {
    if (!indirect_patch_state || src == nullptr || count_buffer == nullptr || max_entries == 0) {
        return {};
    }
    // Both argument layouts are u32 only, the indexed one is five words.
    const u32 words_per_entry = is_indexed ? 5u : 4u;
    const u32 dst_stride_words = words_per_entry;
    const u32 dst_bytes = dst_stride_words * static_cast<u32>(sizeof(u32)) * max_entries;
    const u32 alloc_bytes = Common::AlignUp(dst_bytes, 16u);

    MTL::Buffer* dst_buffer = nullptr;
    u32 dst_offset = 0;
    if (alloc_bytes <= INDIRECT_ARENA_SIZE) {
        const u64 current_tick = scheduler.CurrentTick();
        if (indirect_arena.buffer == nullptr || indirect_arena.tick != current_tick ||
            indirect_arena.used + alloc_bytes > indirect_arena.capacity) {
            const StagingBufferRef chunk =
                staging_pool.Request(INDIRECT_ARENA_SIZE, MemoryUsage::DeviceLocal);
            if (chunk.buffer == nullptr) {
                return {};
            }
            indirect_arena = ScratchArena{
                .buffer = chunk.buffer,
                .base = static_cast<u32>(chunk.offset),
                .used = 0,
                .capacity = INDIRECT_ARENA_SIZE,
                .tick = current_tick,
            };
        }
        dst_buffer = indirect_arena.buffer;
        dst_offset = indirect_arena.base + indirect_arena.used;
        indirect_arena.used += alloc_bytes;
    } else {
        const StagingBufferRef scratch = staging_pool.Request(dst_bytes, MemoryUsage::DeviceLocal);
        if (scratch.buffer == nullptr) {
            return {};
        }
        dst_buffer = scratch.buffer;
        dst_offset = static_cast<u32>(scratch.offset);
    }
    // A stride the guest left at zero means tightly packed.
    const u32 src_stride_words =
        (stride != 0 ? stride : words_per_entry * static_cast<u32>(sizeof(u32))) /
        static_cast<u32>(sizeof(u32));

    struct Info {
        u32 max_entries;
        u32 src_stride_words;
        u32 dst_stride_words;
        u32 words_per_entry;
        u32 count_word;
    } info{max_entries, src_stride_words, dst_stride_words, words_per_entry,
           count_offset / static_cast<u32>(sizeof(u32))};

    MTL::ComputePipelineState* state = indirect_patch_state.get();
    const u32 src_byte_offset = src_offset;
    if (use_upload_compute) {
        // Inputs were written in an earlier submission (or uploaded on the upload
        // encoder, which barriers Blit -> Dispatch). Running on the upload compute
        // encoder keeps the active TBDR render pass open across hundreds of draws.
        scheduler.RecordUploadCompute([state, src, src_byte_offset, count_buffer, dst_buffer,
                                       dst_offset, info, max_entries](ComputeEncoder& encoder) {
            encoder->setComputePipelineState(state);
            encoder->setBuffer(src, src_byte_offset, 0);
            encoder->setBuffer(count_buffer, 0, 1);
            encoder->setBuffer(dst_buffer, dst_offset, 2);
            encoder->setBytes(&info, sizeof(info), 3);
            encoder->dispatchThreads(MTL::Size::Make(max_entries, 1, 1), MTL::Size::Make(64, 1, 1));
        });
    } else {
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([state, src, src_byte_offset, count_buffer, dst_buffer, dst_offset, info,
                          max_entries](CommandContext& context) {
            ComputeEncoder encoder = context.RequestCompute();
            if (encoder == nullptr) {
                return;
            }
            encoder->setComputePipelineState(state);
            encoder->setBuffer(src, src_byte_offset, 0);
            encoder->setBuffer(count_buffer, 0, 1);
            encoder->setBuffer(dst_buffer, dst_offset, 2);
            encoder->setBytes(&info, sizeof(info), 3);
            encoder->dispatchThreads(MTL::Size::Make(max_entries, 1, 1), MTL::Size::Make(64, 1, 1));
            // Order patched indirect arguments before Metal 4's indirect draw command fetch.
            encoder->barrierBeforeQueueStages(
                static_cast<MTL::Stages>(MTL::StageDispatch | MTL::StageBlit), MTL::StageAll);
        });
    }

    return PatchedIndirect{dst_buffer, dst_offset, dst_stride_words * static_cast<u32>(sizeof(u32)),
                           true};
}

ConvertedIndices IndexPass::Convert(Topology topology, IndexFormat format, u32 num_indices,
                                    u32 base_vertex, MTL::Buffer* src, u32 src_offset) {
    const u32 index_size = IndexSize(format);
    if (index_size == 0 || num_indices == 0) {
        return {};
    }

    Mode mode{};
    u32 primitives = 0;
    u32 output = 0;
    switch (topology) {
    case Topology::Quads:
        mode = Mode::Quads;
        primitives = num_indices / 4;
        output = primitives * 6;
        break;
    case Topology::QuadStrip:
        mode = Mode::QuadStrip;
        primitives = num_indices >= 4 ? (num_indices - 2) / 2 : 0;
        output = primitives * 6;
        break;
    case Topology::TriangleFan:
    case Topology::Polygon:
        mode = Mode::Fan;
        primitives = num_indices >= 3 ? num_indices - 2 : 0;
        output = primitives * 3;
        break;
    case Topology::LineLoop:
        mode = Mode::LineLoop;
        // One thread per output index, the last of which closes the loop.
        primitives = num_indices >= 2 ? num_indices + 1 : 0;
        output = primitives;
        break;
    default:
        return {};
    }

    if (primitives == 0) {
        return {};
    }
    return Run(mode, index_size, num_indices, output, primitives, base_vertex, src, src_offset);
}

ConvertedIndices IndexPass::Widen(IndexFormat format, u32 num_indices, u32 base_vertex,
                                  MTL::Buffer* src, u32 src_offset, u32 restart_index,
                                  u32 list_verts_per_prim) {
    const u32 index_size = IndexSize(format);
    if (index_size == 0 || num_indices == 0) {
        return {};
    }
    return Run(Mode::Widen, index_size, num_indices, num_indices, num_indices, base_vertex, src,
               src_offset, restart_index, list_verts_per_prim);
}

u32 IndexPass::VerticesPerPrimitive(Topology topology) {
    switch (topology) {
    case Topology::Points:
        return 1;
    case Topology::Lines:
    case Topology::LineStrip:
        return 2;
    case Topology::Triangles:
    case Topology::TriangleStrip:
    case Topology::TriangleFan:
    case Topology::Polygon:
        return 3;
    case Topology::LinesAdjacency:
    case Topology::LineStripAdjacency:
        return 4;
    case Topology::TrianglesAdjacency:
        return 6;
    default:
        // Quads, quad strips and line loops are already rewritten into triangle
        // or line lists by the buffer cache and never reach here.
        // TriangleStripAdjacency's vertex order is not a sliding window, so it
        // is refused rather than expanded wrongly.
        return 0;
    }
}

u32 IndexPass::PrimitiveCount(Topology topology, u32 count) {
    switch (topology) {
    case Topology::Points:
        return count;
    case Topology::Lines:
        return count / 2;
    case Topology::LineStrip:
        return count >= 2 ? count - 1 : 0;
    case Topology::Triangles:
        return count / 3;
    case Topology::TriangleStrip:
    case Topology::TriangleFan:
    case Topology::Polygon:
        return count >= 3 ? count - 2 : 0;
    case Topology::LinesAdjacency:
        return count / 4;
    case Topology::LineStripAdjacency:
        return count >= 4 ? count - 3 : 0;
    case Topology::TrianglesAdjacency:
        return count / 6;
    default:
        return 0;
    }
}

ConvertedIndices IndexPass::ExpandPrimitives(Topology topology, IndexFormat format,
                                             bool is_indexed, u32 count, u32 base_vertex,
                                             MTL::Buffer* src, u32 src_offset) {
    const u32 verts = VerticesPerPrimitive(topology);
    const u32 primitives = PrimitiveCount(topology, count);
    if (verts == 0 || primitives == 0) {
        return {};
    }
    // Zero means the source is the vertex numbering itself.
    const u32 index_size = is_indexed ? IndexSize(format) : 0;
    if (is_indexed && (index_size == 0 || src == nullptr)) {
        return {};
    }

    ExpandMode expand{};
    switch (topology) {
    case Topology::LineStrip:
    case Topology::LineStripAdjacency:
        expand = ExpandMode::Strip;
        break;
    case Topology::TriangleStrip:
        expand = ExpandMode::TriangleStrip;
        break;
    case Topology::TriangleFan:
    case Topology::Polygon:
        expand = ExpandMode::Fan;
        break;
    default:
        expand = ExpandMode::List;
        break;
    }

    return Run(Mode::Expand, index_size, count, primitives * verts, primitives, base_vertex, src,
               src_offset, 0xFFFFFFFFu, verts, static_cast<u32>(expand));
}

void IndexPass::Fill(MTL::Buffer* dst, u32 offset, u32 num_words, u32 value) {
    if (!pipeline_state || dst == nullptr || num_words == 0) {
        return;
    }
    const Params params{
        .index_size = 4,
        .base_vertex = 0,
        .num_source = num_words,
        .num_threads = num_words,
        .mode = static_cast<u32>(Mode::Fill),
        .value = value,
        .verts_per_prim = 0,
        .expand_mode = 0,
    };
    MTL::ComputePipelineState* const state = pipeline_state.get();
    const u32 groups = Common::DivCeil(num_words, THREADGROUP_SIZE);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([state, dst, offset, params, groups](CommandContext& context) {
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }
        encoder->setComputePipelineState(state);
        // Slot 0 is unused by the fill mode but must still be bound.
        encoder->setBuffer(dst, offset, SOURCE_INDEX);
        encoder->setBuffer(dst, offset, DESTINATION_INDEX);
        encoder->setBytes(&params, sizeof(params), PARAMS_INDEX);
        encoder->dispatchThreadgroups(MTL::Size::Make(groups, 1, 1),
                                      MTL::Size::Make(THREADGROUP_SIZE, 1, 1));
    });
}

ConvertedIndices IndexPass::Run(Mode mode, u32 index_size, u32 num_source, u32 num_output,
                                u32 num_threads, u32 base_vertex, MTL::Buffer* src,
                                u32 src_offset, u32 value, u32 verts_per_prim, u32 expand_mode) {
    if (!pipeline_state || num_output == 0) {
        return {};
    }

    const size_t bytes = static_cast<size_t>(num_output) * sizeof(u32);
    const StagingBufferRef ref = staging_pool.Request(bytes, MemoryUsage::DeviceLocal);
    if (ref.buffer == nullptr) {
        LOG_WARNING(Render_Metal, "No staging space for {} converted indices", num_output);
        return {};
    }

    const Params params{
        .index_size = index_size,
        .base_vertex = base_vertex,
        .num_source = num_source,
        .num_threads = num_threads,
        .mode = static_cast<u32>(mode),
        .value = value,
        .verts_per_prim = verts_per_prim,
        .expand_mode = expand_mode,
    };

    MTL::ComputePipelineState* const state = pipeline_state.get();
    MTL::Buffer* const dst = ref.buffer;
    const u32 dst_offset = static_cast<u32>(ref.offset);
    const u32 groups = Common::DivCeil(num_threads, THREADGROUP_SIZE);
    // Slot 0 still has to hold a buffer, so the destination stands in for it.
    if (src == nullptr) {
        src = dst;
        src_offset = dst_offset;
    }

    // Has to be recorded before the draw that consumes it, so before the render
    // pass opens.
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([state, src, src_offset, dst, dst_offset, params, groups](
                         CommandContext& context) {
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }
        encoder->setComputePipelineState(state);
        encoder->setBuffer(src, src_offset, SOURCE_INDEX);
        encoder->setBuffer(dst, dst_offset, DESTINATION_INDEX);
        encoder->setBytes(&params, sizeof(params), PARAMS_INDEX);
        encoder->dispatchThreadgroups(MTL::Size::Make(groups, 1, 1),
                                      MTL::Size::Make(THREADGROUP_SIZE, 1, 1));
    });

    return ConvertedIndices{
        .buffer = dst,
        .offset = dst_offset,
        .num_indices = num_output,
        .valid = true,
    };
}

} // namespace Metal
