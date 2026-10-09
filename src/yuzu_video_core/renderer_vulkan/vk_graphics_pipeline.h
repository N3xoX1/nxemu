// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "yuzu_common/thread_worker.h"
#include "yuzu_shader_recompiler/shader_info.h"
#include "yuzu_shader_recompiler/vtg_as_compute.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/renderer_vulkan/descriptor_lookup.h"
#include "yuzu_video_core/renderer_vulkan/vk_buffer_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

struct GraphicsPipelineCacheKey {
    std::array<u64, 6> unique_hashes;
    std::array<std::array<u32, 18>, 5> cbuf_sizes;
    FixedPipelineState state;

    size_t Hash() const noexcept;

    bool operator==(const GraphicsPipelineCacheKey& rhs) const noexcept;

    bool operator!=(const GraphicsPipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }

    size_t Size() const noexcept {
        return sizeof(unique_hashes) + sizeof(cbuf_sizes) + state.Size();
    }
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<GraphicsPipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<GraphicsPipelineCacheKey>);

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::GraphicsPipelineCacheKey> {
    size_t operator()(const Vulkan::GraphicsPipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};
} // namespace std

namespace Vulkan {

class Device;
class PipelineStatistics;
class RenderPassCache;
class RescalingPushConstant;
class RenderAreaPushConstant;
class Scheduler;

class StagingBufferPool;

/// Memory for the buffers shared between the stages of the draws running as compute
class VtgScratchAllocator {
public:
    explicit VtgScratchAllocator(const Device& device, Scheduler& scheduler,
                                 StagingBufferPool& staging_pool);

    /// Returns a buffer and the offset of a region that lives until the current tick completes
    [[nodiscard]] std::pair<VkBuffer, VkDeviceSize> Allocate(VkDeviceSize size);

private:
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;
    VkDeviceSize alignment;
    VkBuffer buffer{};
    VkDeviceSize base{};
    VkDeviceSize capacity{};
    VkDeviceSize used{};
    u64 tick{};
};

/// Vertex and geometry stages of a pipeline running as compute before the draw
struct VtgPipelineInfo {
    static constexpr size_t VERTEX = 0;
    static constexpr size_t GEOMETRY = 1;
    static constexpr size_t NUM_COMPUTE_STAGES = 2;

    std::array<vk::ShaderModule, NUM_COMPUTE_STAGES> modules;
    /// Shader information of each stage, including the storage buffers added to it
    std::array<Shader::Info, NUM_COMPUTE_STAGES> infos;
    Shader::VtgVertexBindings vertex_bindings;
    Shader::VtgGeometryBindings geometry_bindings;
    Shader::VtgTopology topology{};
    /// Words of a vertex written by each stage
    u32 vertex_stride{};
    u32 geometry_stride{};
    VtgScratchAllocator* scratch{};
    StagingBufferPool* staging_pool{};
};

class GraphicsPipeline {
    static constexpr size_t NUM_STAGES = Tegra::Engines::Maxwell3D::Regs::MaxShaderStage;

public:
    explicit GraphicsPipeline(
        Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache,
        vk::PipelineCache& pipeline_cache, VideoCore::ShaderNotify* shader_notify,
        const Device& device, DescriptorPool& descriptor_pool,
        GuestDescriptorQueue& guest_descriptor_queue, Common::ThreadWorker* worker_thread,
        PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
        const GraphicsPipelineCacheKey& key, std::array<vk::ShaderModule, NUM_STAGES> stages,
        const std::array<const Shader::Info*, NUM_STAGES>& infos, bool rasterizes_lines,
        std::unique_ptr<VtgPipelineInfo> vtg_info = nullptr);
    ~GraphicsPipeline();

    GraphicsPipeline& operator=(GraphicsPipeline&&) noexcept = delete;
    GraphicsPipeline(GraphicsPipeline&&) noexcept = delete;

    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(const GraphicsPipeline&) = delete;

    void AddTransition(GraphicsPipeline* transition, const GraphicsPipelineCacheKey& transition_key);

    /// Registers the other key served by this pipeline, so both stay on the fast path of Next.
    void SetSharedKey(const GraphicsPipelineCacheKey& shared_key_) noexcept {
        shared_key = shared_key_;
        has_shared_key = true;
    }

    void Configure(bool is_indexed) {
        configure_func(this, is_indexed);
    }

    [[nodiscard]] GraphicsPipeline* Next(const GraphicsPipelineCacheKey& current_key) noexcept {
        if (key == current_key || (has_shared_key && shared_key == current_key)) {
            return this;
        }
        const auto it{std::find(transition_keys.begin(), transition_keys.end(), current_key)};
        return it != transition_keys.end() ? transitions[std::distance(transition_keys.begin(), it)]
                                           : nullptr;
    }

    [[nodiscard]] bool IsBuilt() const noexcept {
        return is_built.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsFailed() const noexcept {
        return build_complete.load(std::memory_order_acquire) && !IsBuilt();
    }

    [[nodiscard]] VkPipelineStageFlags ImageWriteStages() const noexcept {
        return image_write_stages;
    }

    template <typename Spec>
    static auto MakeConfigureSpecFunc() {
        return [](GraphicsPipeline* pl, bool is_indexed) { pl->ConfigureImpl<Spec>(is_indexed); };
    }

    /// Returns true when the vertex and geometry stages run as compute before the draw
    [[nodiscard]] bool IsVtgAsCompute() const noexcept {
        return vtg != nullptr;
    }

    /// Sets the number of instances of the next draw, zero when it cannot run as compute
    void SetVtgInstanceCount(u32 instance_count) noexcept;

    /// Index buffer, its offset and the number of indices drawing the output of the stages
    /// running as compute. The count is zero when there is nothing to draw.
    [[nodiscard]] std::tuple<VkBuffer, VkDeviceSize, u32> VtgDraw() const noexcept;

    void SetEngine(Tegra::Engines::Maxwell3D* maxwell3d_, Tegra::MemoryManager* gpu_memory_) {
        maxwell3d = maxwell3d_;
        gpu_memory = gpu_memory_;
    }

private:
    template <typename Spec>
    void ConfigureImpl(bool is_indexed);

    void ConfigureVtg(bool is_indexed);

    void ConfigureDraw(const RescalingPushConstant& rescaling,
                       const RenderAreaPushConstant& render_are);

    void MakePipeline(VkRenderPass render_pass);

    void Validate();

    const GraphicsPipelineCacheKey key;
    GraphicsPipelineCacheKey shared_key;
    bool has_shared_key{};
    Tegra::Engines::Maxwell3D* maxwell3d;
    Tegra::MemoryManager* gpu_memory;
    const Device& device;
    TextureCache& texture_cache;
    BufferCache& buffer_cache;
    vk::PipelineCache& pipeline_cache;
    Scheduler& scheduler;
    GuestDescriptorQueue& guest_descriptor_queue;

    void (*configure_func)(GraphicsPipeline*, bool){};

    std::vector<GraphicsPipelineCacheKey> transition_keys;
    std::vector<GraphicsPipeline*> transitions;

    std::array<vk::ShaderModule, NUM_STAGES> spv_modules;

    std::array<Shader::Info, NUM_STAGES> stage_infos;
    std::array<u8, Maxwell::NumVertexAttributes> vertex_attribute_components{};
    VkPipelineStageFlags image_write_stages{};
    std::array<u32, 5> enabled_uniform_buffer_masks{};
    VideoCommon::UniformBufferSizes uniform_buffer_sizes{};
    u32 num_textures{};
    u32 num_image_elements{};
    size_t num_descriptor_entries{};

    // Used only by the GPU thread; queued commands use GuestDescriptorQueue's own copies.
    boost::container::small_vector<VideoCommon::ImageViewInOut, 64> descriptor_views;
    boost::container::small_vector<VideoCommon::SamplerId, 64> descriptor_samplers;
    std::unique_ptr<DrawDescriptorCache<VideoCommon::SamplerId>> sampler_lookup;

    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    std::atomic_bool build_complete{false};
    bool uses_push_descriptor{false};
    const bool rasterizes_lines;

    struct VtgState;
    std::unique_ptr<VtgState> vtg;
};

} // namespace Vulkan
