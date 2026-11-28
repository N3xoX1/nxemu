// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "yuzu_common/logging/log.h"
#include <boost/container/small_vector.hpp>

#include "yuzu_common/div_ceil.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_video_core/renderer_metal/mtl_shader_util.h"
#include "yuzu_video_core/renderer_metal/mtl_transform_feedback.h"

namespace Metal {

namespace {

constexpr u32 SOURCE_INDEX = 0;
constexpr u32 DESTINATION_INDEX = 1;
constexpr u32 PARAMS_INDEX = 2;
constexpr u32 THREADGROUP_SIZE = 64;
constexpr u32 VTG_SCAN_WG_SIZE = 256;

/// One varying to move, resolved to plain byte arithmetic.
struct GatherEntry {
    u32 src_offset;  ///< Byte offset within a captured vertex.
    u32 dst_offset;
    u32 dst_stride;
    u32 components;
};

struct Params {
    u32 capture_stride;
    u32 num_vertices;
    u32 num_entries;
    /// Bytes addressable in the destination and the capture, from guest state.
    u32 dst_limit;
    u32 src_limit;
    u32 padding;
};

struct VtgXfbScanParams {
    u32 num_primitives;
    u32 indices_per_prim;
    u32 sentinel;
    u32 output_topology; // 0 = PointList, 1 = LineStrip, 2 = TriangleStrip
};

struct VtgXfbScatterParams {
    u32 num_primitives;
    u32 indices_per_prim;
    u32 sentinel;
    u32 output_topology;
    u32 capture_stride;
    u32 num_entries;
    u32 dst_limit;
    u32 src_limit;
};

struct VtgXfbAdvanceParams {
    u32 active_mask;
    u32 verts_per_prim;
    u32 strides[4];
    u32 pad[2];
};

/// Gathers one varying per thread-y, one vertex per thread-x, plus 3-pass GPU
/// stream compaction + scatter for combined VTG Geometry Shader + Transform Feedback.
constexpr const char* GATHER_MSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GatherEntry {
    uint src_offset;
    uint dst_offset;
    uint dst_stride;
    uint components;
};

struct Params {
    uint capture_stride;
    uint num_vertices;
    uint num_entries;
    uint dst_limit;
    uint src_limit;
    uint padding;
};

struct VtgXfbScanParams {
    uint num_primitives;
    uint indices_per_prim;
    uint sentinel;
    uint output_topology;
};

struct VtgXfbScatterParams {
    uint num_primitives;
    uint indices_per_prim;
    uint sentinel;
    uint output_topology;
    uint capture_stride;
    uint num_entries;
    uint dst_limit;
    uint src_limit;
};

struct XfbGpuCounter {
    ulong bytes;
    ulong primitives;
};

struct VtgXfbAdvanceParams {
    uint active_mask;
    uint verts_per_prim;
    uint strides[4];
    uint pad[2];
};

kernel void gather_varyings(device const uint* capture [[buffer(0)]],
                            device uint* destination [[buffer(1)]],
                            constant Params& p [[buffer(2)]],
                            constant GatherEntry* entries [[buffer(3)]],
                            uint2 id [[thread_position_in_grid]]) {
    if (id.x >= p.num_vertices || id.y >= p.num_entries) {
        return;
    }
    const GatherEntry entry = entries[id.y];
    const uint src = (id.x * p.capture_stride + entry.src_offset) / 4u;
    const uint dst = (id.x * entry.dst_stride + entry.dst_offset) / 4u;
    // Both ends are guest-described, so both are bounds checked: an overrun
    // corrupts whatever buffer follows and eventually faults the GPU.
    for (uint c = 0u; c < entry.components; ++c) {
        if ((dst + c + 1u) * 4u > p.dst_limit || (src + c + 1u) * 4u > p.src_limit) {
            break;
        }
        destination[dst + c] = capture[src + c];
    }
}

static uint CountPrimEmittedVertices(device const uint* indices, uint base, uint count,
                                     uint sentinel, uint topology) {
    if (topology == 0u) {
        uint emitted = 0u;
        for (uint i = 0u; i < count; ++i) {
            if (indices[base + i] < sentinel) {
                ++emitted;
            }
        }
        return emitted;
    }
    uint emitted = 0u;
    uint run = 0u;
    const uint min_run = (topology == 1u) ? 2u : 3u;
    const uint verts_per_out = (topology == 1u) ? 2u : 3u;
    for (uint i = 0u; i < count; ++i) {
        const uint idx = indices[base + i];
        if (idx == 0xFFFFFFFFu || idx >= sentinel) {
            if (run >= min_run) {
                emitted += (run - (min_run - 1u)) * verts_per_out;
            }
            run = 0u;
        } else {
            ++run;
        }
    }
    if (run >= min_run) {
        emitted += (run - (min_run - 1u)) * verts_per_out;
    }
    return emitted;
}

kernel void vtg_xfb_count_scan(device const uint* indices [[buffer(0)]],
                               device uint* prim_offsets [[buffer(1)]],
                               device uint* group_offsets [[buffer(2)]],
                               constant VtgXfbScanParams& p [[buffer(3)]],
                               uint gid [[thread_position_in_grid]],
                               uint lid [[thread_position_in_threadgroup]],
                               uint group_id [[threadgroup_position_in_grid]]) {
    threadgroup uint shared[256];
    const uint val = (gid < p.num_primitives)
                         ? CountPrimEmittedVertices(indices, gid * p.indices_per_prim,
                                                    p.indices_per_prim, p.sentinel,
                                                    p.output_topology)
                         : 0u;
    shared[lid] = val;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint offset = 1u; offset < 256u; offset <<= 1u) {
        const uint add = (lid >= offset) ? shared[lid - offset] : 0u;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        shared[lid] += add;
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (gid < p.num_primitives) {
        prim_offsets[gid] = shared[lid] - val;
    }
    if (lid == 255u) {
        group_offsets[group_id] = shared[255u];
    }
}

kernel void vtg_xfb_scan_groups(device uint* group_offsets [[buffer(0)]],
                                constant uint& num_groups [[buffer(1)]],
                                device uint* total_emitted_out [[buffer(2)]],
                                uint lid [[thread_position_in_threadgroup]]) {
    threadgroup uint shared[256];
    uint running = 0u;
    for (uint base = 0u; base < num_groups; base += 256u) {
        const uint idx = base + lid;
        const uint val = (idx < num_groups) ? group_offsets[idx] : 0u;
        shared[lid] = val;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        for (uint offset = 1u; offset < 256u; offset <<= 1u) {
            const uint add = (lid >= offset) ? shared[lid - offset] : 0u;
            threadgroup_barrier(mem_flags::mem_threadgroup);
            shared[lid] += add;
            threadgroup_barrier(mem_flags::mem_threadgroup);
        }
        if (idx < num_groups) {
            group_offsets[idx] = running + (shared[lid] - val);
        }
        running += shared[255u];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lid == 0u) {
        total_emitted_out[0] = running;
    }
}

static void CopyVtgVertexVaryings(device const uint* geometry_out, device uint* destination,
                                  constant GatherEntry* entries,
                                  constant VtgXfbScatterParams& p, uint src_vertex,
                                  uint dst_vertex, ulong base_bytes) {
    const uint src_base_bytes = src_vertex * p.capture_stride;
    for (uint e = 0u; e < p.num_entries; ++e) {
        const GatherEntry entry = entries[e];
        const uint src_word = (src_base_bytes + entry.src_offset) / 4u;
        const ulong dst_byte =
            base_bytes + ulong(dst_vertex) * ulong(entry.dst_stride) + ulong(entry.dst_offset);
        for (uint c = 0u; c < entry.components; ++c) {
            const ulong out_byte = dst_byte + ulong(c) * 4ul;
            if (out_byte + 4ul > ulong(p.dst_limit) || (src_word + c + 1u) * 4u > p.src_limit) {
                break;
            }
            destination[out_byte / 4ul] = geometry_out[src_word + c];
        }
    }
}

kernel void vtg_xfb_scatter(device const uint* geometry_out [[buffer(0)]],
                            device const uint* indices [[buffer(1)]],
                            device const uint* prim_offsets [[buffer(2)]],
                            device const uint* group_offsets [[buffer(3)]],
                            device uint* destination [[buffer(4)]],
                            constant VtgXfbScatterParams& p [[buffer(5)]],
                            constant GatherEntry* entries [[buffer(6)]],
                            device const XfbGpuCounter* counter [[buffer(7)]],
                            uint prim [[thread_position_in_grid]]) {
    if (prim >= p.num_primitives) {
        return;
    }
    const ulong base_bytes = counter[0].bytes;
    uint dst_v = prim_offsets[prim] + group_offsets[prim >> 8u];
    const uint base = prim * p.indices_per_prim;
    if (p.output_topology == 0u) {
        for (uint i = 0u; i < p.indices_per_prim; ++i) {
            const uint v = indices[base + i];
            if (v < p.sentinel) {
                CopyVtgVertexVaryings(geometry_out, destination, entries, p, v, dst_v++,
                                      base_bytes);
            }
        }
    } else if (p.output_topology == 1u) {
        uint prev = 0xFFFFFFFFu;
        for (uint i = 0u; i < p.indices_per_prim; ++i) {
            const uint v = indices[base + i];
            if (v == 0xFFFFFFFFu || v >= p.sentinel) {
                prev = 0xFFFFFFFFu;
            } else {
                if (prev != 0xFFFFFFFFu) {
                    CopyVtgVertexVaryings(geometry_out, destination, entries, p, prev, dst_v++,
                                          base_bytes);
                    CopyVtgVertexVaryings(geometry_out, destination, entries, p, v, dst_v++,
                                          base_bytes);
                }
                prev = v;
            }
        }
    } else {
        uint v0 = 0xFFFFFFFFu;
        uint v1 = 0xFFFFFFFFu;
        uint tri_idx = 0u;
        for (uint i = 0u; i < p.indices_per_prim; ++i) {
            const uint v = indices[base + i];
            if (v == 0xFFFFFFFFu || v >= p.sentinel) {
                v0 = 0xFFFFFFFFu;
                v1 = 0xFFFFFFFFu;
                tri_idx = 0u;
            } else {
                if (v0 != 0xFFFFFFFFu && v1 != 0xFFFFFFFFu) {
                    if ((tri_idx & 1u) == 0u) {
                        CopyVtgVertexVaryings(geometry_out, destination, entries, p, v0, dst_v++,
                                              base_bytes);
                        CopyVtgVertexVaryings(geometry_out, destination, entries, p, v1, dst_v++,
                                              base_bytes);
                        CopyVtgVertexVaryings(geometry_out, destination, entries, p, v, dst_v++,
                                              base_bytes);
                    } else {
                        CopyVtgVertexVaryings(geometry_out, destination, entries, p, v1, dst_v++,
                                              base_bytes);
                        CopyVtgVertexVaryings(geometry_out, destination, entries, p, v0, dst_v++,
                                              base_bytes);
                        CopyVtgVertexVaryings(geometry_out, destination, entries, p, v, dst_v++,
                                              base_bytes);
                    }
                    ++tri_idx;
                }
                v0 = v1;
                v1 = v;
            }
        }
    }
}

kernel void vtg_xfb_advance_counters(device const uint* total_emitted [[buffer(0)]],
                                     device XfbGpuCounter* counters [[buffer(1)]],
                                     constant VtgXfbAdvanceParams& p [[buffer(2)]]) {
    const uint emitted_verts = total_emitted[0];
    const uint vpp = max(p.verts_per_prim, 1u);
    const ulong emitted_prims = ulong(emitted_verts / vpp);
    for (uint s = 0u; s < 4u; ++s) {
        if (((p.active_mask >> s) & 1u) != 0u) {
            counters[s].bytes += ulong(emitted_verts) * ulong(p.strides[s]);
            counters[s].primitives += emitted_prims;
        }
    }
}
)MSL";

/// Attribute word of gl_Position, and of the first generic output.
constexpr u32 POSITION_WORD = 28;
constexpr u32 GENERIC_BASE_WORD = 32;

} // Anonymous namespace

TransformFeedback::TransformFeedback(const Device& device_, Scheduler& scheduler_,
                                     MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_} {
    ScopedAutoreleasePool pool;

    NS::Error* error = nullptr;
    MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
    library = Adopt(device.GetDevice()->newLibrary(ToNSString(GATHER_MSL), options, &error));
    options->release();
    if (!library) {
        LOG_ERROR(Render_Metal, "Failed to compile the transform feedback gather: {}",
                  DescribeError(error));
        return;
    }
    library->setLabel(MTLSTR("nxemu transform feedback"));

    gather_pipeline = BuildComputePipeline(device, library.get(), "gather_varyings",
                                           "nxemu transform feedback gather");
    vtg_count_scan_pipeline = BuildComputePipeline(device, library.get(), "vtg_xfb_count_scan",
                                                   "nxemu vtg xfb count scan");
    vtg_scan_groups_pipeline = BuildComputePipeline(device, library.get(), "vtg_xfb_scan_groups",
                                                    "nxemu vtg xfb scan groups");
    vtg_scatter_pipeline = BuildComputePipeline(device, library.get(), "vtg_xfb_scatter",
                                                "nxemu vtg xfb scatter");
    vtg_advance_counters_pipeline = BuildComputePipeline(
        device, library.get(), "vtg_xfb_advance_counters", "nxemu vtg xfb advance counters");
}

TransformFeedback::~TransformFeedback() = default;

Owned<MTL::Buffer> TransformFeedback::CreateCaptureSegment(u32 bytes) {
    if (bytes == 0) {
        return {};
    }
    Owned<MTL::Buffer> buffer = memory_allocator.CreateBuffer(bytes, MemoryUsage::DeviceLocal);
    if (buffer) {
        buffer->setLabel(MTLSTR("nxemu xfb capture segment"));
    } else {
        LOG_WARNING(Render_Metal, "Could not allocate a {} byte capture segment", bytes);
    }
    return buffer;
}

MTL::Buffer* TransformFeedback::AcquireCaptureSegment(u32 bytes) {
    if (bytes == 0) {
        return nullptr;
    }
    // Bucketed, so captures that vary by a few vertices reuse one segment.
    u32 capacity = 4096;
    while (capacity < bytes) {
        capacity *= 2;
    }

    const MasterSemaphore& semaphore = scheduler.GetMasterSemaphore();
    for (PooledSegment& segment : segment_pool) {
        if (segment.held || segment.capacity < capacity ||
            !semaphore.IsFree(segment.free_after)) {
            continue;
        }
        segment.held = true;
        // A capture only writes the varying slots its shader outputs, and a stale
        // float in a slot the gather consumes is a vertex flung to infinity. On
        // the CPU because a GPU fill was left undone by encoder ordering.
        if (void* const contents = segment.buffer->contents()) {
            std::memset(contents, 0, bytes);
        }
        return segment.buffer.get();
    }

    Owned<MTL::Buffer> buffer =
        memory_allocator.CreateBuffer(capacity, MemoryUsage::DeviceLocal);
    if (!buffer) {
        LOG_WARNING(Render_Metal, "Could not allocate a {} byte capture segment", capacity);
        return nullptr;
    }
    buffer->setLabel(MTLSTR("nxemu xfb capture segment"));

    MTL::Buffer* const handle = buffer.get();
    segment_pool.push_back(PooledSegment{
        .buffer = std::move(buffer),
        .capacity = capacity,
        .held = true,
    });
    if (segment_pool.size() > logged_pool_high_water && segment_pool.size() % 64 == 0) {
        logged_pool_high_water = segment_pool.size();
        LOG_INFO(Render_Metal, "Transform feedback capture pool grew to {} segments",
                 segment_pool.size());
    }
    return handle;
}

void TransformFeedback::ReleaseCaptureSegment(MTL::Buffer* segment) {
    if (segment == nullptr) {
        return;
    }
    for (PooledSegment& pooled : segment_pool) {
        if (pooled.buffer.get() == segment) {
            // NOT stamped with a tick here, on purpose. Captured vertices are
            // read by draws recorded after the capture, so the current tick is no
            // upper bound on the work referencing this segment and neither is a
            // fixed margin on it. Reuse it too early and a later draw reads a
            // segment the GPU is overwriting: a mesh exploding across the screen.
            pooled.held = false;
            pooled.free_after = UNSTAMPED;
            return;
        }
    }
}

void TransformFeedback::StampReleasedSegments() {
    // Nothing more will be recorded against these, so the next tick bounds them.
    const MasterSemaphore& semaphore = scheduler.GetMasterSemaphore();
    const u64 tick = semaphore.CurrentTick();
    for (PooledSegment& pooled : segment_pool) {
        if (!pooled.held && pooled.free_after == UNSTAMPED) {
            pooled.free_after = tick;
        }
    }
    // Trim excess idle segments above the high-water mark so a single frame spike
    // does not pin wired residency allocations for the entire session, while keeping
    // enough warm segments for XFB-heavy titles (e.g. 64-128 captures/frame).
    constexpr size_t MAX_POOLED_SEGMENTS = 256;
    if (segment_pool.size() > MAX_POOLED_SEGMENTS) {
        for (auto it = segment_pool.begin();
             it != segment_pool.end() && segment_pool.size() > MAX_POOLED_SEGMENTS;) {
            if (!it->held && it->free_after != UNSTAMPED && semaphore.IsFree(it->free_after)) {
                if (it->buffer) {
                    scheduler.KeepAlive(it->buffer.get(), it->buffer.get());
                }
                it = segment_pool.erase(it);
            } else {
                ++it;
            }
        }
    }
}

MTL::Buffer* TransformFeedback::CaptureBuffer(u32 bytes) {
    if (bytes == 0) {
        return nullptr;
    }
    if (capture_buffer && capture_capacity >= bytes) {
        return capture_buffer.get();
    }
    if (capture_buffer) {
        scheduler.KeepAlive(capture_buffer.get(), capture_buffer.get());
    }
    const u32 capacity = std::max(bytes, capture_capacity * 2);
    capture_buffer = memory_allocator.CreateBuffer(capacity, MemoryUsage::DeviceLocal);
    if (!capture_buffer) {
        LOG_WARNING(Render_Metal, "Could not allocate a {} byte transform feedback capture",
                    capacity);
        capture_capacity = 0;
        return nullptr;
    }
    capture_buffer->setLabel(MTLSTR("nxemu xfb capture"));
    capture_capacity = capacity;
    return capture_buffer.get();
}

void TransformFeedback::Gather(MTL::Buffer* capture, u32 capture_stride, u32 num_vertices,
                               std::span<const Shader::TransformFeedbackVarying> varyings,
                               std::span<const TransformFeedbackBinding> bindings,
                               std::span<const s8> slot_of_generic, s32 position_slot) {
    if (!gather_pipeline || capture == nullptr || num_vertices == 0 || capture_stride == 0) {
        return;
    }

    // Byte offset of attribute word @p word inside a captured vertex, or -1.
    // SPIRV-Cross compacts stored generics and puts position last.
    const auto capture_offset_of = [&](u32 word) -> s32 {
        if (word >= POSITION_WORD && word < POSITION_WORD + 4) {
            return position_slot < 0
                       ? -1
                       : static_cast<s32>(position_slot * 16 + (word - POSITION_WORD) * 4);
        }
        if (word < GENERIC_BASE_WORD) {
            return -1;
        }
        const u32 generic = (word - GENERIC_BASE_WORD) / 4;
        const u32 component = (word - GENERIC_BASE_WORD) % 4;
        if (generic >= slot_of_generic.size() || slot_of_generic[generic] < 0) {
            return -1;
        }
        return static_cast<s32>(slot_of_generic[generic] * 16 + component * 4);
    };

    // The guest describes its capture as a sparse table indexed by attribute
    // word. One dispatch per destination buffer, of which there can be several.
    MTL::ComputePipelineState* const state = gather_pipeline.get();
    const u32 groups = Common::DivCeil(num_vertices, THREADGROUP_SIZE);

    for (size_t buffer_index = 0; buffer_index < bindings.size(); ++buffer_index) {
        if (bindings[buffer_index].buffer == nullptr) {
            continue;
        }
        boost::container::small_vector<GatherEntry, 32> entries;
        for (u32 word = 0; word < varyings.size(); ++word) {
            const Shader::TransformFeedbackVarying& varying = varyings[word];
            if (varying.components == 0 || varying.buffer != buffer_index) {
                continue;
            }
            const s32 src = capture_offset_of(word);
            if (src < 0) {
                continue;
            }
            entries.push_back(GatherEntry{
                .src_offset = static_cast<u32>(src),
                .dst_offset = varying.offset,
                .dst_stride = varying.stride,
                .components = varying.components,
            });
        }
        if (entries.empty()) {
            continue;
        }

        const Params params{
            .capture_stride = capture_stride,
            .num_vertices = num_vertices,
            .num_entries = static_cast<u32>(entries.size()),
            .dst_limit = bindings[buffer_index].size,
            .src_limit = capture_stride * num_vertices,
            .padding = 0,
        };
        MTL::Buffer* const destination = bindings[buffer_index].buffer;
        const u32 destination_offset = bindings[buffer_index].offset;

        // Keep both buffers alive until the GPU finishes executing this tick.
        // On Metal 4, command buffers do not retain referenced resources, so a
        // later regrow/eviction would otherwise free them while still in flight.
        scheduler.KeepAlive(capture);
        scheduler.KeepAlive(destination);
        scheduler.Record([state, capture, destination, destination_offset, params, groups,
                          entries = std::move(entries)](CommandContext& context) {
            ComputeEncoder encoder = context.RequestCompute();
            if (encoder == nullptr) {
                return;
            }
            encoder->setComputePipelineState(state);
            encoder->setBuffer(capture, 0, SOURCE_INDEX);
            encoder->setBuffer(destination, destination_offset, DESTINATION_INDEX);
            encoder->setBytes(&params, sizeof(params), PARAMS_INDEX);
            encoder->setBytes(entries.data(), entries.size() * sizeof(GatherEntry), 3);
            encoder->dispatchThreadgroups(MTL::Size::Make(groups, params.num_entries, 1),
                                          MTL::Size::Make(THREADGROUP_SIZE, 1, 1));
        });
    }
}

bool TransformFeedback::GatherVtg(StagingBufferPool& staging_pool,
                                  const StagingBufferRef& geometry_out, u32 geometry_stride_bytes,
                                  u32 max_output_vertices, const StagingBufferRef& index_out,
                                  u32 num_primitives, u32 indices_per_primitive, u32 sentinel,
                                  Shader::OutputTopology output_topology,
                                  const Shader::VtgVaryingLayout& geometry_layout,
                                  const VideoCommon::TransformFeedbackState& xfb_state,
                                  std::span<const TransformFeedbackBinding> bindings,
                                  MTL::Buffer* counter_buffer, u32 active_counter_mask) {
    if (!vtg_count_scan_pipeline || !vtg_scan_groups_pipeline || !vtg_scatter_pipeline ||
        !vtg_advance_counters_pipeline || geometry_out.buffer == nullptr ||
        index_out.buffer == nullptr || num_primitives == 0 || indices_per_primitive == 0 ||
        geometry_stride_bytes == 0) {
        return false;
    }

    // Build per-buffer gather tables directly from live guest stream_out_layout word-by-word so
    // swizzled, unaligned, repeated, or per-draw-changing XFB layouts are copied bit-exactly.
    std::array<boost::container::small_vector<GatherEntry, 32>, 4> per_buffer_entries;
    bool any_destination = false;
    const size_t num_buffers = std::min<size_t>(bindings.size(), per_buffer_entries.size());
    for (size_t buffer_index = 0; buffer_index < num_buffers; ++buffer_index) {
        if (bindings[buffer_index].buffer == nullptr || bindings[buffer_index].size == 0) {
            continue;
        }
        const auto& layout = xfb_state.layouts[buffer_index];
        if (layout.stream != 0 || layout.stride == 0 || layout.varying_count == 0) {
            continue;
        }
        const auto& locations = xfb_state.varyings[buffer_index];
        const u32 word_count = std::min<u32>({layout.varying_count, layout.stride / 4u,
                                              static_cast<u32>(locations.size() * 4u)});
        auto& entries = per_buffer_entries[buffer_index];
        for (u32 word = 0; word < word_count; ++word) {
            const auto& loc = locations[word / 4u];
            u32 attribute = 0;
            switch (word % 4u) {
            case 0:
                attribute = loc.attribute0.Value();
                break;
            case 1:
                attribute = loc.attribute1.Value();
                break;
            case 2:
                attribute = loc.attribute2.Value();
                break;
            case 3:
                attribute = loc.attribute3.Value();
                break;
            }
            const std::optional<u32> src_word =
                geometry_layout.Offset(static_cast<Shader::IR::Attribute>(attribute));
            if (!src_word) {
                continue;
            }
            const u32 src_offset = *src_word * 4u;
            const u32 dst_offset = word * 4u;
            if (!entries.empty() && entries.back().dst_stride == layout.stride &&
                entries.back().src_offset + entries.back().components * 4u == src_offset &&
                entries.back().dst_offset + entries.back().components * 4u == dst_offset) {
                ++entries.back().components;
            } else {
                entries.push_back(GatherEntry{
                    .src_offset = src_offset,
                    .dst_offset = dst_offset,
                    .dst_stride = layout.stride,
                    .components = 1u,
                });
            }
        }
        if (!entries.empty()) {
            any_destination = true;
        }
    }
    if (!any_destination) {
        return false;
    }

    const u32 num_groups = Common::DivCeil(num_primitives, VTG_SCAN_WG_SIZE);
    const StagingBufferRef prim_offsets =
        staging_pool.Request(u64{num_primitives} * sizeof(u32), MemoryUsage::DeviceLocal);
    const StagingBufferRef group_offsets =
        staging_pool.Request(u64{num_groups} * sizeof(u32), MemoryUsage::DeviceLocal);
    if (prim_offsets.buffer == nullptr || group_offsets.buffer == nullptr) {
        LOG_WARNING(Render_Metal, "VTG XFB scratch allocation failed ({} primitives)",
                    num_primitives);
        return false;
    }

    const u32 topology_code = [&]() -> u32 {
        switch (output_topology) {
        case Shader::OutputTopology::PointList:
            return 0u;
        case Shader::OutputTopology::LineStrip:
            return 1u;
        case Shader::OutputTopology::TriangleStrip:
            return 2u;
        }
        return 0u;
    }();

    scheduler.KeepAlive(geometry_out.buffer);
    scheduler.KeepAlive(index_out.buffer);
    scheduler.KeepAlive(prim_offsets.buffer);
    scheduler.KeepAlive(group_offsets.buffer);

    const VtgXfbScanParams scan_params{
        .num_primitives = num_primitives,
        .indices_per_prim = indices_per_primitive,
        .sentinel = sentinel,
        .output_topology = topology_code,
    };

    MTL::ComputePipelineState* const count_scan_state = vtg_count_scan_pipeline.get();
    scheduler.Record([count_scan_state, index_buf = index_out.buffer, index_off = index_out.offset,
                      prim_buf = prim_offsets.buffer, prim_off = prim_offsets.offset,
                      group_buf = group_offsets.buffer, group_off = group_offsets.offset,
                      scan_params, num_groups](CommandContext& context) {
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }
        encoder->setComputePipelineState(count_scan_state);
        encoder->setBuffer(index_buf, index_off, 0);
        encoder->setBuffer(prim_buf, prim_off, 1);
        encoder->setBuffer(group_buf, group_off, 2);
        encoder->setBytes(&scan_params, sizeof(scan_params), 3);
        encoder->dispatchThreadgroups(MTL::Size::Make(num_groups, 1, 1),
                                      MTL::Size::Make(VTG_SCAN_WG_SIZE, 1, 1));
    });

    const StagingBufferRef total_emitted =
        staging_pool.Request(sizeof(u32), MemoryUsage::DeviceLocal);
    if (total_emitted.buffer == nullptr) {
        return false;
    }
    MTL::Buffer* const total_buf = total_emitted.buffer;
    const u32 total_off = static_cast<u32>(total_emitted.offset);
    scheduler.KeepAlive(total_buf);

    StagingBufferRef fallback_counter{};
    MTL::Buffer* active_counter_buf = counter_buffer;
    u32 counter_base_off = 0;
    bool counter_has_per_stream_slots = counter_buffer != nullptr;
    if (active_counter_buf == nullptr) {
        fallback_counter = staging_pool.Request(sizeof(XfbGpuCounter), MemoryUsage::Upload);
        if (fallback_counter.buffer == nullptr) {
            return false;
        }
        std::memset(fallback_counter.mapped_span.data(), 0, sizeof(XfbGpuCounter));
        active_counter_buf = fallback_counter.buffer;
        counter_base_off = static_cast<u32>(fallback_counter.offset);
    }
    scheduler.KeepAlive(active_counter_buf);

    MTL::ComputePipelineState* const scan_groups_state = vtg_scan_groups_pipeline.get();
    scheduler.Record([scan_groups_state, group_buf = group_offsets.buffer,
                      group_off = group_offsets.offset, num_groups, total_buf,
                      total_off](CommandContext& context) {
        ComputeEncoder encoder = context.RequestCompute();
        if (encoder == nullptr) {
            return;
        }
        encoder->setComputePipelineState(scan_groups_state);
        encoder->setBuffer(group_buf, group_off, 0);
        encoder->setBytes(&num_groups, sizeof(num_groups), 1);
        encoder->setBuffer(total_buf, total_off, 2);
        encoder->dispatchThreadgroups(MTL::Size::Make(1, 1, 1),
                                      MTL::Size::Make(VTG_SCAN_WG_SIZE, 1, 1));
    });

    MTL::ComputePipelineState* const scatter_state = vtg_scatter_pipeline.get();
    const u32 scatter_groups = Common::DivCeil(num_primitives, THREADGROUP_SIZE);
    const u64 src_limit_u64 = u64{max_output_vertices} * geometry_stride_bytes;
    const u32 src_limit =
        static_cast<u32>(std::min<u64>(src_limit_u64, std::numeric_limits<u32>::max()));

    for (size_t buffer_index = 0; buffer_index < num_buffers; ++buffer_index) {
        auto& entries = per_buffer_entries[buffer_index];
        if (entries.empty()) {
            continue;
        }
        const VtgXfbScatterParams scatter_params{
            .num_primitives = num_primitives,
            .indices_per_prim = indices_per_primitive,
            .sentinel = sentinel,
            .output_topology = topology_code,
            .capture_stride = geometry_stride_bytes,
            .num_entries = static_cast<u32>(entries.size()),
            .dst_limit = bindings[buffer_index].size,
            .src_limit = src_limit,
        };
        MTL::Buffer* const destination = bindings[buffer_index].buffer;
        const u32 destination_offset = bindings[buffer_index].offset;
        const u32 slot_counter_off =
            counter_has_per_stream_slots
                ? static_cast<u32>(buffer_index * sizeof(XfbGpuCounter))
                : counter_base_off;
        scheduler.KeepAlive(destination);
        scheduler.Record([scatter_state, geom_buf = geometry_out.buffer,
                          geom_off = geometry_out.offset, index_buf = index_out.buffer,
                          index_off = index_out.offset, prim_buf = prim_offsets.buffer,
                          prim_off = prim_offsets.offset, group_buf = group_offsets.buffer,
                          group_off = group_offsets.offset, destination, destination_offset,
                          active_counter_buf, slot_counter_off, scatter_params, scatter_groups,
                          entries = std::move(entries)](CommandContext& context) {
            ComputeEncoder encoder = context.RequestCompute();
            if (encoder == nullptr) {
                return;
            }
            encoder->setComputePipelineState(scatter_state);
            encoder->setBuffer(geom_buf, geom_off, 0);
            encoder->setBuffer(index_buf, index_off, 1);
            encoder->setBuffer(prim_buf, prim_off, 2);
            encoder->setBuffer(group_buf, group_off, 3);
            encoder->setBuffer(destination, destination_offset, 4);
            encoder->setBytes(&scatter_params, sizeof(scatter_params), 5);
            encoder->setBytes(entries.data(), entries.size() * sizeof(GatherEntry), 6);
            encoder->setBuffer(active_counter_buf, slot_counter_off, 7);
            encoder->dispatchThreadgroups(MTL::Size::Make(scatter_groups, 1, 1),
                                          MTL::Size::Make(THREADGROUP_SIZE, 1, 1));
        });
    }

    if (counter_buffer != nullptr && active_counter_mask != 0) {
        const u32 verts_per_prim =
            output_topology == Shader::OutputTopology::PointList
                ? 1u
                : (output_topology == Shader::OutputTopology::LineStrip ? 2u : 3u);
        VtgXfbAdvanceParams advance_params{
            .active_mask = active_counter_mask,
            .verts_per_prim = verts_per_prim,
            .strides = {xfb_state.layouts[0].stride, xfb_state.layouts[1].stride,
                        xfb_state.layouts[2].stride, xfb_state.layouts[3].stride},
            .pad = {0, 0},
        };
        MTL::ComputePipelineState* const advance_state = vtg_advance_counters_pipeline.get();
        scheduler.Record([advance_state, total_buf, total_off, counter_buffer,
                          advance_params](CommandContext& context) {
            ComputeEncoder encoder = context.RequestCompute();
            if (encoder == nullptr) {
                return;
            }
            encoder->setComputePipelineState(advance_state);
            encoder->setBuffer(total_buf, total_off, 0);
            encoder->setBuffer(counter_buffer, 0, 1);
            encoder->setBytes(&advance_params, sizeof(advance_params), 2);
            encoder->dispatchThreadgroups(MTL::Size::Make(1, 1, 1), MTL::Size::Make(1, 1, 1));
        });
    }
    return true;
}

} // namespace Metal
