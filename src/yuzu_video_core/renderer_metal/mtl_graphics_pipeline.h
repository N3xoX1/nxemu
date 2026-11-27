// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/thread_worker.h"
#include "yuzu_shader_recompiler/runtime_info.h"
#include "yuzu_shader_recompiler/shader_info.h"
#include "yuzu_shader_recompiler/vtg_as_compute.h"
#include "yuzu_video_core/buffer_cache/buffer_cache_base.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/renderer_metal/fixed_pipeline_state.h"
#include "yuzu_video_core/renderer_metal/mtl_shader_util.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Metal {

class Device;
class Scheduler;

/// Index into the per-stage arrays. Geometry never contributes a function to
/// the render pipeline (it drives a compute dispatch), but it still needs its
/// constant buffers, textures and argument buffers resolved per-stage.
enum class PipelineStage : size_t {
    Vertex = 0,
    Fragment = 1,
    Geometry = 2,
    Count = 3,
};

constexpr size_t NUM_PIPELINE_STAGES = static_cast<size_t>(PipelineStage::Count);

/// Stages the render pipeline itself is built from.
constexpr size_t NUM_RENDER_PIPELINE_STAGES = 2;

/// Maxwell program index backing each Metal stage. The buffer cache is indexed
/// by Maxwell stage (VertexB 0, Geometry 3, Fragment 4) and not by PipelineStage.
/// The wrong index silently feeds a shader another stage's constant buffers.
constexpr std::array<size_t, NUM_PIPELINE_STAGES> MAXWELL_STAGE_INDEX{0, 4, 3};

// Everything a geometry-emulated pipeline needs beyond an ordinary one. The
// guest vertex program becomes a compute kernel writing its outputs to a buffer,
// the geometry program becomes a second kernel expanding those into triangles,
// and the render pipeline's vertex function is a generated passthrough reading
// the result. Buffer layouts are fixed by Shader::Backend::SPIRV's
// geometry-as-compute ABI.
struct GeometryEmulation {
    /// The guest vertex program compiled with capture_output_to_buffer.
    MslShader capture;
    /// The geometry program, already lowered to GLCompute by the recompiler.
    MslShader geometry;

    /// float4s per captured vertex: one per stored generic location, then
    /// gl_Position. Must agree with EmitContext::gs_input_stride.
    u32 input_stride{};
    /// Vertices one input primitive is assembled from.
    u32 input_vertices{};
    /// float4s per emitted vertex, i.e. GEOMETRY_AS_COMPUTE_OUTPUT_STRIDE.
    u32 output_stride{};
    /// Vertices one thread may emit, i.e. GeometryAsComputeOutputCapacity().
    u32 output_capacity{};

    /// Argument-buffer slots the geometry kernel declares, for the same drift
    /// check the render stages get. (The vertex stage's count is the capture
    /// kernel's, since the passthrough consumes nothing.)
    u32 geometry_argument_slots{};

    /// Transform feedback capture riding the capture-kernel machinery. When set,
    /// `geometry` is empty and the guest vertex program still runs as the
    /// pipeline's real vertex function. The capture kernel just also writes the
    /// vertex stream out to the guest's feedback buffers.
    bool xfb_only{};
    /// Capture-buffer float4 slot per generic location, -1 when the vertex
    /// program does not store it. SPIRV-Cross packs stored generics in
    /// ascending location order, then gl_Position last.
    std::array<s8, 32> xfb_slot_of_generic{};
    s8 xfb_position_slot{-1};
    /// The guest's capture table, indexed by attribute word.
    std::array<Shader::TransformFeedbackVarying, 256> xfb_varyings{};
    u32 xfb_varying_count{};
};

/// IR-level vertex+geometry compute emulation (shared with the Vulkan/MoltenVK
/// workaround): VtgVertexToCompute runs the guest vertex stage into a scratch
/// buffer, VtgGeometryToCompute runs the geometry stage and emits vertices plus
/// a 32-bit strip/point index buffer, and GenerateVtgPassthroughVertex feeds
/// the emitted vertices into the Metal render pipeline.
struct VtgEmulation {
    MslShader vertex_kernel;
    MslShader geometry_kernel;
    Shader::VtgVertexBindings vertex_bindings;
    Shader::VtgGeometryBindings geometry_bindings;
    Shader::VtgTopology topology{};
    u32 vertex_stride{};
    u32 geometry_stride{};
    bool uses_constant_attributes{};
    u8 constant_binding{};

    /// Transform feedback capture directly from the emulated geometry stage's output.
    bool xfb_enabled{};
    Shader::VtgVaryingLayout geometry_layout{};

    // Indirect draw capacity sizing & GPU feedback ({requested, truncated}).
    mutable u64 largest_draw{};
    mutable bool has_largest_draw{};
    mutable u32 indirect_capacity{};
    mutable Owned<MTL::Buffer> feedback_buffer;
};

/// A compiled MTLRenderPipelineState, built asynchronously, plus the emulated
/// geometry/transform-feedback kernels that go with it. Depth/stencil state is
/// kept out of here on purpose, it lives in DepthStencilCache below so a changed
/// depth comparison rebuilds only that and not the shader pipeline.
class GraphicsPipeline : Common::NonCopyable, Common::NonMoveable {
public:
    /// @param worker_thread Pool the MSL compile runs on. nullptr builds
    ///        synchronously (blocking preload path only).
    /// @param geometry Non-null only when the geometry stage or XFB is emulated.
    /// @param vtg Non-null when using IR-level VTG compute emulation.
    explicit GraphicsPipeline(const Device& device, Scheduler& scheduler,
                              ShaderWorkerPool* worker_thread,
                              const FixedPipelineState& state,
                              std::array<const Shader::Info*, NUM_PIPELINE_STAGES> infos,
                              std::array<const MslShader*, NUM_PIPELINE_STAGES> shaders,
                              VideoCore::ShaderNotify* shader_notify = nullptr,
                              std::unique_ptr<GeometryEmulation> geometry = nullptr,
                              std::unique_ptr<VtgEmulation> vtg = nullptr);
    ~GraphicsPipeline();

    /// Whether this pipeline is worth binding. Optimistic: an in-flight build
    /// reports valid, only a finished-and-failed one is invalid. The draw path
    /// re-checks Handle() after WaitBuilt().
    [[nodiscard]] bool IsValid() const noexcept {
        return !build_failed.load(std::memory_order::acquire);
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order::acquire);
    }

    /// Blocks until the asynchronous build finishes. Call from the scheduler's
    /// worker thread, never from the thread recording draws.
    void WaitBuilt() const;

    /// Null until the build finishes, and stays null if it failed.
    [[nodiscard]] MTL::RenderPipelineState* Handle() const noexcept {
        return pipeline_state.get();
    }

    [[nodiscard]] const FixedPipelineState& State() const noexcept {
        return key;
    }

    /// Whether the fragment program writes colour output @p index. Maxwell leaves a target
    /// the shader does not write untouched, but the MSL output struct declares and
    /// zero-fills every output, so such an attachment has to be masked off.
    [[nodiscard]] static bool StoresColor(const FixedPipelineState& key,
                                          const Shader::Info* fragment_info, u32 index);

    /// Whether any stage writes memory other than its attachments: storage buffers, storage
    /// images or global memory. Only such writes need a guest barrier to end the pass.
    [[nodiscard]] bool HasStorageWrites() const noexcept {
        return has_storage_writes;
    }

    /// Whether the vertex stage writes memory (SSBOs, storage images, global memory).
    /// A rasterizer-discard draw with vertex storage writes must still execute its vertex stage.
    [[nodiscard]] bool HasVertexStorageWrites() const noexcept {
        return has_vertex_storage_writes;
    }

    /// Whether the vertex stage samples/reads textures or images that a prior
    /// render pass's attachments may have written.
    [[nodiscard]] bool VertexReadsTextures() const noexcept {
        return vertex_reads_textures;
    }

    /// Whether the vertex stage reads SSBOs, texel buffers, or global memory
    /// that a prior render pass's fragment storage writes may have modified.
    [[nodiscard]] bool VertexReadsBuffers() const noexcept {
        return vertex_reads_buffers;
    }

    /// Precomputed colour attachment write mask (bit i set when write_mask != None and
    /// fragment shader stores output i).
    [[nodiscard]] u32 WrittenColorAttachments() const noexcept {
        return written_color_attachments;
    }

    /// Precomputed bitmask of enabled vertex buffer binding slots.
    [[nodiscard]] u32 EnabledVertexSlots() const noexcept {
        return enabled_vertex_slots;
    }

    [[nodiscard]] const Shader::Info* StageInfo(PipelineStage stage) const noexcept {
        const auto& info = stage_infos[static_cast<size_t>(stage)];
        return info ? &*info : nullptr;
    }

    /// Constant-buffer enable masks, indexed by *Maxwell* stage, for
    /// BufferCache::SetUniformBuffersState.
    [[nodiscard]] const std::array<u32, VideoCommon::NUM_STAGES>&
    EnabledUniformBufferMasks() const noexcept {
        return enabled_uniform_buffer_masks;
    }

    [[nodiscard]] const VideoCommon::UniformBufferSizes& UniformBufferSizes() const noexcept {
        return uniform_buffer_sizes;
    }

    /// Argument-buffer slots SPIRV-Cross emitted for @p stage. Zero when the
    /// stage is absent or binds nothing.
    [[nodiscard]] u32 ExpectedArgumentSlots(PipelineStage stage) const noexcept {
        return expected_argument_slots[static_cast<size_t>(stage)];
    }

    /// DescriptorSet 1 table argument-buffer slots for @p stage. Zero when unused.
    [[nodiscard]] u32 ExpectedTableArgumentSlots(PipelineStage stage) const noexcept {
        return expected_table_argument_slots[static_cast<size_t>(stage)];
    }

    /// Whether the guest disabled rasterization, or asked to cull both faces
    /// (no such Metal cull mode). Either way the draw skips rasterization.
    [[nodiscard]] bool IsRasterizerDiscard() const noexcept {
        return key.rasterizer_discard;
    }

    /// Non-null when this draw runs through geometry emulation. Readable
    /// before the build finishes; only the pipeline states need WaitBuilt().
    [[nodiscard]] const GeometryEmulation* GeometryParams() const noexcept {
        return geometry.get();
    }

    /// Non-null when this draw runs through IR-level VTG compute emulation.
    [[nodiscard]] const VtgEmulation* VtgParams() const noexcept {
        return vtg.get();
    }

    /// True when a single non-indexed indirect draw can keep its arguments on the GPU.
    [[nodiscard]] bool CanDrawVtgIndirect() const noexcept {
        if (!vtg || !vtg->has_largest_draw) {
            return false;
        }
        if (!vtg->feedback_buffer) {
            return true;
        }
        auto* const words = static_cast<u32*>(vtg->feedback_buffer->contents());
        if (words == nullptr) {
            return false;
        }
        return std::atomic_ref<u32>(words[1]).load(std::memory_order_relaxed) == 0;
    }

    /// Whether the guest declared a geometry stage this pipeline runs without,
    /// so the draw path can report it per draw rather than per compile.
    void SetDroppedGeometry() noexcept {
        dropped_geometry = true;
    }
    [[nodiscard]] bool DroppedGeometry() const noexcept {
        return dropped_geometry;
    }

    /// The guest vertex program as a capture kernel (for transform feedback).
    /// Null until the build finishes, and stays null if it failed.
    [[nodiscard]] MTL::ComputePipelineState* CaptureHandle() const noexcept {
        return capture_pipeline.get();
    }

    /// IR-level VTG vertex compute kernel.
    [[nodiscard]] MTL::ComputePipelineState* VtgVertexHandle() const noexcept {
        return vtg_vertex_pipeline.get();
    }

    /// IR-level VTG geometry compute kernel.
    [[nodiscard]] MTL::ComputePipelineState* VtgGeometryHandle() const noexcept {
        return vtg_geometry_pipeline.get();
    }

private:
    void Build(std::array<MslShader, NUM_PIPELINE_STAGES> shaders,
               std::array<bool, NUM_PIPELINE_STAGES> present);
    /// Builds the compute pipelines an emulated geometry stage needs. Failing
    /// either fails the whole pipeline.
    [[nodiscard]] bool BuildGeometryPipelines();
    [[nodiscard]] bool BuildVtgPipelines();
    [[nodiscard]] Owned<MTL::VertexDescriptor> BuildVertexDescriptor() const;
    /// The capture kernel's equivalent of the vertex descriptor. Mirrors
    /// MTLVertexDescriptor except step functions run against the dispatch grid.
    /// Unused right now, the capture kernel fetches its attributes manually
    /// (InjectManualVertexFetch) instead of taking them through stage_in.
    [[nodiscard]] Owned<MTL::StageInputOutputDescriptor> BuildStageInputDescriptor() const;
    void SignalBuilt();

    const Device& device;
    Scheduler& scheduler;
    /// Non-null while this pipeline still owes a MarkShaderComplete. Cleared by
    /// SignalBuilt so the pairing happens exactly once however Build ended.
    std::atomic<VideoCore::ShaderNotify*> shader_notify{};
    FixedPipelineState key;

    // Held by value: the caller's Shader::Info lives in a local that dies when
    // CreateGraphicsPipeline returns, so pointers into it would dangle.
    std::array<std::optional<Shader::Info>, NUM_PIPELINE_STAGES> stage_infos{};
    std::array<u32, NUM_PIPELINE_STAGES> expected_argument_slots{};
    std::array<u32, NUM_PIPELINE_STAGES> expected_table_argument_slots{};
    std::array<u32, VideoCommon::NUM_STAGES> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    bool has_storage_writes{};
    bool has_vertex_storage_writes{};
    bool vertex_reads_textures{};
    bool vertex_reads_buffers{};
    u32 written_color_attachments{};
    u32 enabled_vertex_slots{};

    Owned<MTL::RenderPipelineState> pipeline_state;

    /// Null unless this pipeline captures transform feedback.
    std::unique_ptr<GeometryEmulation> geometry;
    std::unique_ptr<VtgEmulation> vtg;
    bool dropped_geometry{};
    Owned<MTL::ComputePipelineState> capture_pipeline;
    Owned<MTL::ComputePipelineState> vtg_vertex_pipeline;
    Owned<MTL::ComputePipelineState> vtg_geometry_pipeline;

    mutable std::mutex build_mutex;
    mutable std::condition_variable build_condvar;
    std::atomic_bool is_built{false};
    std::atomic_bool build_failed{false};
};

/// Builds (and owns) MTLDepthStencilState objects, keyed by their description.
class DepthStencilCache : Common::NonCopyable, Common::NonMoveable {
public:
    explicit DepthStencilCache(const Device& device);
    ~DepthStencilCache();

    [[nodiscard]] MTL::DepthStencilState* Get(const DepthStencilState& state);

private:
    const Device& device;
    std::unordered_map<DepthStencilState, Owned<MTL::DepthStencilState>> cache;
    bool has_last{};
    DepthStencilState last_state{};
    MTL::DepthStencilState* last_result{};
};

} // namespace Metal
