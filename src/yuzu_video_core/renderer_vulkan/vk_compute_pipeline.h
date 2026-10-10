// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "yuzu_common/common_types.h"
#include "yuzu_common/thread_worker.h"
#include "yuzu_shader_recompiler/shader_info.h"
#include "yuzu_video_core/renderer_vulkan/descriptor_lookup.h"
#include "yuzu_video_core/renderer_vulkan/vk_buffer_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore {
class ShaderNotify;
}

namespace Vulkan {

class Device;
class PipelineStatistics;
class Scheduler;

class ComputePipeline {
public:
    explicit ComputePipeline(const Device& device, vk::PipelineCache& pipeline_cache,
                             DescriptorPool& descriptor_pool,
                             GuestDescriptorQueue& guest_descriptor_queue,
                             Common::ThreadWorker* thread_worker,
                             PipelineStatistics* pipeline_statistics,
                             VideoCore::ShaderNotify* shader_notify, const Shader::Info& info,
                             vk::ShaderModule spv_module, StagingBufferPool& staging_pool);

    ComputePipeline& operator=(ComputePipeline&&) noexcept = delete;
    ComputePipeline(ComputePipeline&&) noexcept = delete;

    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(const ComputePipeline&) = delete;

    void Configure(Tegra::Engines::KeplerCompute& kepler_compute, Tegra::MemoryManager& gpu_memory,
                   Scheduler& scheduler, BufferCache& buffer_cache, TextureCache& texture_cache);
    void FinishRuntimeImageWrites(Scheduler& scheduler, TextureCache& texture_cache);

    /// Returns true when the host rejected the pipeline. Only valid once the build has finished.
    [[nodiscard]] bool HasBuildFailed() const noexcept {
        return build_failed.load(std::memory_order::relaxed);
    }

private:
    const Device& device;
    vk::PipelineCache& pipeline_cache;
    GuestDescriptorQueue& guest_descriptor_queue;
    Shader::Info info;
    StagingBufferPool& staging_pool;
    StagingBufferRef runtime_image_write_mask{};
    std::vector<VideoCommon::ImageViewId> runtime_image_views;
    u64 runtime_image_dispatch_count{};
    u32 num_textures{};
    size_t num_descriptor_entries{};

    boost::container::small_vector<VideoCommon::ImageViewInOut, 64> descriptor_views;
    boost::container::small_vector<VideoCommon::SamplerId, 64> descriptor_samplers;
    std::unique_ptr<DrawDescriptorCache<VideoCommon::SamplerId>> sampler_lookup;

    VideoCommon::ComputeUniformBufferSizes uniform_buffer_sizes{};

    vk::ShaderModule spv_module;
    vk::DescriptorSetLayout descriptor_set_layout;
    DescriptorAllocator descriptor_allocator;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    vk::Pipeline pipeline;

    std::condition_variable build_condvar;
    std::mutex build_mutex;
    std::atomic_bool is_built{false};
    std::atomic_bool build_failed{false};
};

} // namespace Vulkan
