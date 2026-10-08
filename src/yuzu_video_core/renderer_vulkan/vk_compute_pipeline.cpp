// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <vector>

#include "yuzu_video_core/renderer_vulkan/pipeline_helper.h"
#include "yuzu_video_core/renderer_vulkan/pipeline_statistics.h"
#include "yuzu_video_core/renderer_vulkan/vk_buffer_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/shader_notify.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

using Shader::ImageBufferDescriptor;
using Shader::Backend::SPIRV::RESCALING_LAYOUT_WORDS_OFFSET;
using Tegra::Texture::TexturePair;

ComputePipeline::ComputePipeline(const Device& device_, vk::PipelineCache& pipeline_cache_,
                                 DescriptorPool& descriptor_pool,
                                 GuestDescriptorQueue& guest_descriptor_queue_,
                                 Common::ThreadWorker* thread_worker,
                                 PipelineStatistics* pipeline_statistics,
                                 VideoCore::ShaderNotify* shader_notify, const Shader::Info& info_,
                                 vk::ShaderModule spv_module_)
    : device{device_},
      pipeline_cache(pipeline_cache_), guest_descriptor_queue{guest_descriptor_queue_}, info{info_},
      spv_module(std::move(spv_module_)) {
    num_descriptor_entries = NumDescriptorUpdateEntries(info);
    num_textures = Shader::NumDescriptors(info.texture_descriptors);
    descriptor_views.resize(num_textures + Shader::NumDescriptors(info.texture_buffer_descriptors) +
                            Shader::NumDescriptors(info.image_buffer_descriptors) +
                            Shader::NumDescriptors(info.image_descriptors));
    descriptor_samplers.resize(num_textures);
    if (shader_notify) {
        shader_notify->MarkShaderBuilding();
    }
    std::copy_n(info.constant_buffer_used_sizes.begin(), uniform_buffer_sizes.size(),
                uniform_buffer_sizes.begin());

    auto func{[this, &descriptor_pool, shader_notify, pipeline_statistics] {
        DescriptorLayoutBuilder builder{device};
        builder.Add(info, VK_SHADER_STAGE_COMPUTE_BIT);

        descriptor_set_layout = builder.CreateDescriptorSetLayout(false);
        pipeline_layout = builder.CreatePipelineLayout(*descriptor_set_layout);
        descriptor_update_template =
            builder.CreateTemplate(*descriptor_set_layout, *pipeline_layout, false);
        descriptor_allocator = descriptor_pool.Allocator(*descriptor_set_layout, info);
        const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup_size_ci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
            .pNext = nullptr,
            .requiredSubgroupSize = GuestWarpSize,
        };
        VkPipelineCreateFlags flags{};
        if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
            flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        }
        pipeline = device.GetLogical().CreateComputePipeline(
            {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .pNext = nullptr,
                .flags = flags,
                .stage{
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .pNext =
                        device.CanRequestComputeSubgroupSize() ? &subgroup_size_ci : nullptr,
                    .flags = 0,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = *spv_module,
                    .pName = "main",
                    .pSpecializationInfo = nullptr,
                },
                .layout = *pipeline_layout,
                .basePipelineHandle = 0,
                .basePipelineIndex = 0,
            },
            *pipeline_cache);

        if (pipeline_statistics) {
            pipeline_statistics->Collect(*pipeline);
        }
        std::scoped_lock lock{build_mutex};
        is_built = true;
        build_condvar.notify_one();
        if (shader_notify) {
            shader_notify->MarkShaderComplete();
        }
    }};
    if (thread_worker) {
        thread_worker->QueueWork(std::move(func));
    } else {
        func();
    }
}

void ComputePipeline::Configure(Tegra::Engines::KeplerCompute& kepler_compute,
                                Tegra::MemoryManager& gpu_memory, Scheduler& scheduler,
                                BufferCache& buffer_cache, TextureCache& texture_cache) {
    guest_descriptor_queue.Acquire(num_descriptor_entries);

    buffer_cache.SetComputeUniformBufferState(info.constant_buffer_mask, &uniform_buffer_sizes);
    buffer_cache.UnbindComputeStorageBuffers();
    size_t ssbo_index{};
    for (const auto& desc : info.storage_buffers_descriptors) {
        ASSERT(desc.count == 1);
        buffer_cache.BindComputeStorageBuffer(ssbo_index, desc.cbuf_index, desc.cbuf_offset,
                                              desc.is_written);
        ++ssbo_index;
    }

    texture_cache.SynchronizeComputeDescriptors();

    auto& views = descriptor_views;
    auto& samplers = descriptor_samplers;
    size_t view_index{};
    size_t sampler_index{};
    if (num_textures > 64) {
        if (!sampler_lookup) {
            sampler_lookup = std::make_unique<DrawDescriptorCache<VideoCommon::SamplerId>>();
        }
        sampler_lookup->Reset();
    }
    const bool memoize_samplers = sampler_lookup && sampler_lookup->Enabled();
    const auto lookup_sampler = [&](u32 index) {
        return texture_cache.GetComputeSamplerId(index);
    };

    const auto& qmd{kepler_compute.launch_description};
    const auto& cbufs{qmd.const_buffer_config};
    DescriptorCbufReader<8> cbuf_reader;
    const auto bind_cbuf = [&](u32 bank, u32 count) {
        return cbuf_reader.Bind(gpu_memory, bank, cbufs[bank].Address(), cbufs[bank].size.Value(), count);
    };
    const bool via_header_index{qmd.linked_tsc != 0};
    const auto prepare_handle{[&](const auto& desc) {
        ASSERT(((qmd.const_buffer_enable_mask >> desc.cbuf_index) & 1) != 0);
        const auto primary = bind_cbuf(desc.cbuf_index, desc.count);
        auto secondary = primary;
        bool has_secondary{};
        u32 secondary_offset{}, primary_shift{}, secondary_shift{};
        if constexpr (requires { desc.has_secondary; }) {
            if (desc.has_secondary) {
                ASSERT(((qmd.const_buffer_enable_mask >> desc.secondary_cbuf_index) & 1) != 0);
                secondary = bind_cbuf(desc.secondary_cbuf_index, desc.count);
                has_secondary = true;
                secondary_offset = desc.secondary_cbuf_offset;
                primary_shift = desc.shift_left;
                secondary_shift = desc.secondary_shift_left;
            }
        }
        return [=](u32 index) {
            const u32 delta = index << desc.size_shift;
            const u32 raw = primary.Read(desc.cbuf_offset + delta);
            return TexturePair(has_secondary ? (raw << primary_shift) |
                                (secondary.Read(secondary_offset + delta) << secondary_shift) : raw,
                               via_header_index);
        };
    }};
    const auto add_image{[&](const auto& desc, bool blacklist) {
        const auto read_handle = prepare_handle(desc);
        for (u32 index = 0; index < desc.count; ++index) {
            const auto handle{read_handle(index)};
            views[view_index++] = {
                .index = handle.first,
                .blacklist = blacklist,
                .id = {},
            };
        }
    }};
    for (const auto& desc : info.texture_buffer_descriptors) {
        add_image(desc, false);
    }
    for (const auto& desc : info.image_buffer_descriptors) {
        add_image(desc, false);
    }
    for (const auto& desc : info.texture_descriptors) {
        const auto read_handle = prepare_handle(desc);
        for (u32 index = 0; index < desc.count; ++index) {
            const auto handle{read_handle(index)};
            views[view_index++] = {handle.first};

            const VideoCommon::SamplerId sampler = memoize_samplers
                ? sampler_lookup->Get(handle.second, lookup_sampler)
                : lookup_sampler(handle.second);
            samplers[sampler_index++] = sampler;
        }
    }
    for (const auto& desc : info.image_descriptors) {
        add_image(desc, desc.is_written);
    }
    ASSERT(view_index == views.size());
    ASSERT(sampler_index == samplers.size());
    texture_cache.FillComputeImageViews(std::span(views.data(), views.size()));

    buffer_cache.UnbindComputeTextureBuffers();
    size_t index{};
    const auto add_buffer{[&](const auto& desc) {
        constexpr bool is_image = std::is_same_v<decltype(desc), const ImageBufferDescriptor&>;
        for (u32 i = 0; i < desc.count; ++i) {
            bool is_written{false};
            if constexpr (is_image) {
                is_written = desc.is_written;
            }
            ImageView& image_view = texture_cache.GetImageView(views[index].id);
            buffer_cache.BindComputeTextureBuffer(index, image_view.GpuAddr(),
                                                  image_view.BufferSize(), image_view.format,
                                                  is_written, is_image);
            ++index;
        }
    }};
    std::ranges::for_each(info.texture_buffer_descriptors, add_buffer);
    std::ranges::for_each(info.image_buffer_descriptors, add_buffer);

    buffer_cache.UpdateComputeBuffers();
    buffer_cache.BindHostComputeBuffers();

    RescalingPushConstant rescaling;
    const VideoCommon::SamplerId* samplers_it{samplers.data()};
    const VideoCommon::ImageViewInOut* views_it{views.data()};
    PushImageDescriptors(texture_cache, guest_descriptor_queue, info, rescaling, samplers_it,
                         views_it);

    if (!is_built.load(std::memory_order::relaxed)) {
        // Wait for the pipeline to be built
        scheduler.Record([this](vk::CommandBuffer) {
            std::unique_lock lock{build_mutex};
            build_condvar.wait(lock, [this] { return is_built.load(std::memory_order::relaxed); });
        });
    }
    const void* const descriptor_data{guest_descriptor_queue.UpdateData()};
    const bool is_rescaling = !info.texture_descriptors.empty() || !info.image_descriptors.empty();
    scheduler.Record([this, descriptor_data, is_rescaling,
                      rescaling_data = rescaling.Data()](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        if (!descriptor_set_layout) {
            return;
        }
        if (is_rescaling) {
            cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                 RESCALING_LAYOUT_WORDS_OFFSET, sizeof(rescaling_data),
                                 rescaling_data.data());
        }
        const VkDescriptorSet descriptor_set{descriptor_allocator.Commit()};
        const vk::Device& dev{device.GetLogical()};
        dev.UpdateDescriptorSet(descriptor_set, *descriptor_update_template, descriptor_data);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline_layout, 0,
                                  descriptor_set, nullptr);
    });
}

} // namespace Vulkan
