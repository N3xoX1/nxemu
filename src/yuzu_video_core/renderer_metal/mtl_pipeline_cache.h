// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <array>
#include <filesystem>
#include <memory>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/thread_worker.h"
#include "yuzu_shader_recompiler/frontend/ir/basic_block.h"
#include "yuzu_shader_recompiler/frontend/maxwell/control_flow.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate_program.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/object_pool.h"
#include "yuzu_shader_recompiler/profile.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/renderer_metal/fixed_pipeline_state.h"
#include "yuzu_video_core/renderer_metal/mtl_compute_pipeline.h"
#include "yuzu_video_core/renderer_metal/mtl_graphics_pipeline.h"
#include "yuzu_video_core/renderer_vulkan/cbuf_size_dependencies.h"
#include "yuzu_video_core/shader_cache.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Metal {

class Device;
class Scheduler;

/// Key for a graphics pipeline, the shader hashes plus the baked state. Packed
/// because Hash() digests it bytewise and the shader cache writes it to disk the
/// same way. With padding two equal keys can hash differently.
#pragma pack(push, 1)
struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes{};
    FixedPipelineState state{};
    /// Constant buffer reads an HLE macro turns into draw parameters, one bit per
    /// HLEReplacementAttributeType. The shader is compiled differently with them.
    u32 hle_replacements{};
    /// Bound cbuf byte sizes for banks queried by TexturePass (0 for non-dependent banks).
    std::array<std::array<u32, 18>, 5> cbuf_sizes{};
    /// Guest texture/sampler pool limits when a stage queries ReadTexturePoolInfo().
    std::array<u32, 2> texture_pool{};

    /// Bytewise: the key is packed, and memberwise comparison of the nested
    /// state showed up per draw.
    [[nodiscard]] bool operator==(const GraphicsPipelineCacheKey& other) const noexcept {
        return std::memcmp(this, &other, sizeof(*this)) == 0;
    }

    [[nodiscard]] size_t Hash() const noexcept;
};
#pragma pack(pop)

static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>,
              "The graphics key is hashed and serialized bytewise, so it must not have padding");

/// Same packing requirement as the graphics key, hashed over sizeof(*this) and
/// read from the disk cache straight into the struct.
#pragma pack(push, 1)
struct ComputePipelineCacheKey {
    u64 unique_hash{};
    u32 shared_memory_size{};
    std::array<u32, 3> workgroup_size{};
    std::array<u32, 8> cbuf_sizes{};
    u32 texture_table_size{};

    [[nodiscard]] bool operator==(const ComputePipelineCacheKey&) const noexcept = default;

    [[nodiscard]] size_t Hash() const noexcept;
};
#pragma pack(pop)

static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>,
              "The compute key is hashed and serialized bytewise, so it must not have padding");

} // namespace Metal

namespace std {

template <>
struct hash<Metal::GraphicsPipelineCacheKey> {
    size_t operator()(const Metal::GraphicsPipelineCacheKey& key) const noexcept {
        return key.Hash();
    }
};

template <>
struct hash<Metal::ComputePipelineCacheKey> {
    size_t operator()(const Metal::ComputePipelineCacheKey& key) const noexcept {
        return key.Hash();
    }
};

} // namespace std

namespace Metal {

/// Whether @p factor references the shader's second colour output.
[[nodiscard]] bool IsDualSourceBlendFactor(
    Tegra::Engines::Maxwell3D::Regs::Blend::Factor factor);

/// Whether attachment 0 blends against a second source colour. Shared with the
/// rasterizer, it changes the fragment shader's declared outputs so both the
/// recompiler and the pipeline key need it.
[[nodiscard]] bool ComputeAttachment0DualSourceBlend(
    const Tegra::Engines::Maxwell3D::Regs& regs);

/// How the shader has to declare vertex input @p index, given what the vertex
/// descriptor will hand it. The rasterizer calls this while building the
/// pipeline key, see the definition for why the two have to agree.
[[nodiscard]] Shader::AttributeType AttributeTypeOf(
    const Tegra::Engines::Maxwell3D::Regs& regs, size_t index);

/// Compiles and caches Metal pipelines from guest shader programs. Shaped like
/// the Vulkan pipeline cache. Its Shader::Profile turns off capabilities Metal
/// doesn't have, which makes the recompiler emit its emulation paths.
class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory, const Device& device,
                           Scheduler& scheduler, VideoCore::ShaderNotify& shader_notify);
    ~PipelineCache();

    /// Pipeline for the current graphics state, null while it's still compiling
    /// or if the compile failed.
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline();

    /// Waits for the pipeline of the current draw when CurrentGraphicsPipeline skipped it
    /// only because its build had not finished. Returns null otherwise.
    [[nodiscard]] GraphicsPipeline* WaitForCurrentGraphicsPipeline();

    /// True when the last CurrentGraphicsPipeline skipped a pipeline whose build had not
    /// finished yet.
    [[nodiscard]] bool SkippedUnfinished() const noexcept {
        return skipped_unfinished;
    }

    /// Null when @p pipeline is still compiling and we can skip the draw instead
    /// of waiting on it. See use_asynchronous_shaders.
    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) const noexcept;

    /// Returns the pipeline for the current compute state, or null.
    [[nodiscard]] ComputePipeline* CurrentComputePipeline();

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

    [[nodiscard]] DepthStencilCache& GetDepthStencilCache() noexcept {
        return depth_stencil_cache;
    }

    /// State the rasterizer fills in before asking for a pipeline.
    void SetGraphicsState(const FixedPipelineState& state) {
        graphics_state = state;
        ++graphics_state_generation;
    }

private:
    /// Arena pools for one translation. Released between pipelines.
    struct ShaderPools {
        void ReleaseContents() {
            flow_block.ReleaseContents();
            block.ReleaseContents();
            inst.ReleaseContents();
        }

        Shader::ObjectPool<Shader::IR::Inst> inst{8192};
        Shader::ObjectPool<Shader::IR::Block> block{32};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
    };

    /// Full off-thread graphics translation + compilation job (IR -> SPIR-V -> MSL -> PSO).
    struct PendingGraphicsPipeline {
        GraphicsPipelineCacheKey key;
        GraphicsEnvironments environments;
        std::unique_ptr<GraphicsPipeline> pipeline;
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
    };

    [[nodiscard]] bool CanSkipCurrentDraw() const noexcept;
    [[nodiscard]] bool QueueGraphicsPipeline(const GraphicsPipelineCacheKey& key);
    void DrainCompletedGraphicsPipelines();
    void FinishPendingGraphics(const GraphicsPipelineCacheKey& key);
    void SerializeGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                   GraphicsEnvironments& environments);
    void NormalizeGraphicsCbufSizes(GraphicsPipelineCacheKey& key) const;
    void ObserveGraphicsEnvironments(const GraphicsPipelineCacheKey& key,
                                     const GraphicsEnvironments& environments);

    /// build_in_parallel hands the MSL compile to the worker pool. False for the
    /// disk loader, it's already on a worker and needs the pipeline finished
    /// before it counts it as built.
    [[nodiscard]] std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        const GraphicsEnvironments& envs, bool build_in_parallel = true);

    /// Builds a compute pipeline from @p env instead of the live registers,
    /// that's what lets the disk loader preload compute at all.
    /// build_in_parallel hands SPIRV-Cross + MSL + PSO creation to the worker pool.
    [[nodiscard]] std::unique_ptr<ComputePipeline> CreateComputePipeline(
        ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
        bool build_in_parallel = true);

    const Device& device;
    Scheduler& scheduler;
    /// Drives the frontend's "Building N shader(s)" indicator. Pair every build,
    /// a MarkShaderBuilding with no MarkShaderComplete leaves the counter stuck
    /// above zero for the rest of the session.
    VideoCore::ShaderNotify& shader_notify;

    /// Whether a draw can be SKIPPED while its pipeline is still compiling
    /// instead of stalling the scheduler thread in WaitBuilt. Read once, we
    /// check it per draw and the setting must not change mid-session.
    const bool use_asynchronous_shaders;
    mutable bool skipped_unfinished{false};
    bool wait_for_current_draw{false};

    // Version stamped into the shader cache file. MUST be bumped whenever the
    // pipeline key layout or the translation changes. Entries are [env...][key]
    // with no length prefix, so a key that grew or shrank by one byte desyncs
    // the reader and every entry after it is parsed misaligned and preloaded
    // under a nonsense key. The guard in LoadPipelines only catches absurd
    // values, not plausible garbage.
    //
    // 2: tessellation domain state added to FixedPipelineState.
    // 3: constant_binding added to FixedPipelineState.
    // 4: depth_format/stencil_format removed from FixedPipelineState. Metal 4
    //    has no depth setter on the pipeline descriptor, so they only
    //    multiplied the pipeline count.
    // 5: VertexBinding gained `instanced`, an instanced stream at frequency
    //    zero is a third state a divisor alone can't express. A version-4 cache
    //    read through the new layout gives garbage strides and step functions,
    //    which draws as screen-wide smeared geometry, the exact bug the field
    //    was added to fix.
    // 6: hle_replacements added to the graphics key.
    // 7: unwritten texture descriptors no longer translate as 1D.
    // 8: direct bindless separate-sampler tables enabled (Set 1).
    // 9: cbuf_sizes and texture_pool/texture_table_size added to pipeline keys
    //    (matching Vulkan's CbufSizeDependencies).
    /// Byte size of the on-disk graphics key. A literal on purpose, see the
    /// static_assert below.
    static constexpr size_t GRAPHICS_KEY_SIZE = 1188;
    static constexpr u32 CACHE_VERSION = 9;

    /// Tripwire for the above, and it's a LITERAL on purpose. It used to be
    /// `48 + sizeof(FixedPipelineState)`, which is tautological, it tracked the
    /// struct it was meant to police. Adding a field there changed the on-disk
    /// layout in silence and every existing cache got misread. If this fires,
    /// bump CACHE_VERSION *and* the number, never just the number.
    static_assert(sizeof(GraphicsPipelineCacheKey) == GRAPHICS_KEY_SIZE,
                  "The graphics key layout changed - bump CACHE_VERSION above");

    std::filesystem::path pipeline_cache_filename;


    Shader::Profile profile;
    /// Render stages inline the global memory helpers: DontInline there makes
    /// TOTK flicker (seen through MoltenVK), only compute builds need it.
    Shader::Profile graphics_profile;
    Shader::HostTranslateInfo host_info;

    FixedPipelineState graphics_state{};

    GraphicsPipelineCacheKey graphics_key{};
    Vulkan::CbufSizeDependencies cbuf_size_dependencies;
    Vulkan::GraphicsCbufSizeState<5, 18> graphics_cbuf_size_state;

    ComputePipelineCacheKey compute_key{};

    GraphicsPipeline* current_graphics_pipeline{};

    /// Pipelines drawn right after a given one, so most draws find theirs by
    /// comparing a few keys instead of hashing one. Entries of an unordered_map
    /// stay where they are, and graphics_cache never erases.
    struct GraphicsTransition {
        const GraphicsPipelineCacheKey* key;
        std::unique_ptr<GraphicsPipeline>* pipeline;
    };
    std::unordered_map<const void*, std::vector<GraphicsTransition>> graphics_transitions;
    const void* last_graphics_entry{};
    /// Bumped by SetGraphicsState. A lookup made at the current generation, with the
    /// same stages, answers the next one without comparing keys.
    u64 graphics_state_generation{1};
    u64 looked_up_generation{};

    std::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>>
        graphics_cache;
    std::unordered_map<GraphicsPipelineCacheKey, std::shared_ptr<PendingGraphicsPipeline>>
        pending_graphics;
    std::mutex completed_graphics_mutex;
    std::vector<GraphicsPipelineCacheKey> completed_graphics;
    std::atomic<bool> has_completed_graphics{false};
    VideoCommon::GraphicsEnvironment::SnapshotCache snapshot_cache;

    std::unordered_map<ComputePipelineCacheKey, std::unique_ptr<ComputePipeline>> compute_cache;
    /// How many times we retried a kernel that failed to build. See
    /// CurrentComputePipeline, a build can fail just because the guest hasn't
    /// written the code yet and a later dispatch gets it right.
    std::unordered_map<ComputePipelineCacheKey, u32> compute_failures;

    DepthStencilCache depth_stencil_cache;

    ShaderPools main_pools;

    /// Worker pool for shader translation and compilation, spawned with an 8 MiB
    /// stack so SPIRV-Cross runs inline without helper-thread hops.
    std::unique_ptr<ShaderWorkerPool> workers;

    /// Writes cache entries off the draw path, the caller is mid-frame. Declared
    /// last so the construction order matches the initialiser list.
    Common::StatefulThreadWorker<> serialization_thread;

    /// Flushes dirty Metal 4 archive(s) periodically once the worker pool goes idle.
    void MaybeFlushMetal4Archive(bool graphics_added, bool compute_added);
    /// Serialization-thread only, so these need no synchronisation.
    u32 graphics_pipelines_since_archive_flush{};
    u32 compute_pipelines_since_archive_flush{};
    std::chrono::steady_clock::time_point last_archive_flush{};
};

} // namespace Metal
