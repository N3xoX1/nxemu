// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <span>

#include <boost/container/static_vector.hpp>

#include "yuzu_video_core/renderer_vulkan/pipeline_helper.h"

#include "yuzu_common/bit_field.h"
#include "yuzu_video_core/renderer_vulkan/maxwell_to_vk.h"
#include "yuzu_video_core/renderer_vulkan/pipeline_statistics.h"
#include "yuzu_video_core/renderer_vulkan/vk_buffer_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "yuzu_video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/shader_notify.h"
#include "yuzu_video_core/texture_cache/texture_cache.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "nxemu-video/video_settings.h"

#if defined(_MSC_VER) && defined(NDEBUG)
#define LAMBDA_FORCEINLINE [[msvc::forceinline]]
#else
#define LAMBDA_FORCEINLINE
#endif

namespace Vulkan {
namespace {
using boost::container::static_vector;
using Shader::ImageBufferDescriptor;
using Shader::Backend::SPIRV::RENDERAREA_LAYOUT_OFFSET;
using Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET;
using Shader::Backend::SPIRV::RESCALING_LAYOUT_WORDS_OFFSET;
using Tegra::Texture::TexturePair;
using VideoCore::Surface::PixelFormat;
using VideoCore::Surface::PixelFormatFromDepthFormat;
using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

constexpr size_t NUM_STAGES = Maxwell::MaxShaderStage;
constexpr size_t INLINE_IMAGE_ELEMENTS = 64;

DescriptorLayoutBuilder MakeBuilder(const Device& device, std::span<const Shader::Info> infos,
                                    u32 num_color_attachments) {
    static constexpr std::array stages{
        VK_SHADER_STAGE_VERTEX_BIT,
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
        VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
        VK_SHADER_STAGE_GEOMETRY_BIT,
        VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkShaderStageFlags storage_buffer_stages{};
    if (device.GetDriverID() == VK_DRIVER_ID_MOLTENVK) {
        // MoltenVK can lose the resource usage of a storage buffer reused across shader stages
        // (KhronosGroup/MoltenVK#1870). Like Ryubing, make every storage buffer binding visible
        // to all graphics stages using descriptors, as long as the widened layout stays within
        // the per-stage limits.
        std::array<u32, NUM_STAGES> stage_resources{};
        std::array<u32, NUM_STAGES> stage_storage_buffers{};
        u32 num_storage_buffers{};
        for (size_t index = 0; index < infos.size(); ++index) {
            const auto& info = infos[index];
            const u32 storage_buffers = Shader::NumDescriptors(info.storage_buffers_descriptors);
            const u32 resources = Shader::NumDescriptors(info.constant_buffer_descriptors) +
                                  storage_buffers +
                                  Shader::NumDescriptors(info.texture_buffer_descriptors) +
                                  Shader::NumDescriptors(info.image_buffer_descriptors) +
                                  Shader::NumDescriptors(info.texture_descriptors) +
                                  Shader::NumDescriptors(info.image_descriptors);
            stage_resources[index] = resources;
            stage_storage_buffers[index] = storage_buffers;
            num_storage_buffers += storage_buffers;
            if (resources != 0) {
                storage_buffer_stages |= stages.at(index);
            }
        }
        bool can_widen = num_storage_buffers != 0 &&
                         num_storage_buffers <= device.GetMaxPerStageStorageBuffers();
        for (size_t index = 0; can_widen && index < infos.size(); ++index) {
            if ((storage_buffer_stages & stages.at(index)) == 0) {
                continue;
            }
            u32 resources =
                stage_resources[index] - stage_storage_buffers[index] + num_storage_buffers;
            if (stages.at(index) == VK_SHADER_STAGE_FRAGMENT_BIT) {
                // Color attachments also count against maxPerStageResources.
                resources += num_color_attachments;
                const u64 storage_images =
                    u64{Shader::NumDescriptors(infos[index].image_descriptors)} +
                    Shader::NumDescriptors(infos[index].image_buffer_descriptors);
                can_widen = storage_images + num_storage_buffers + num_color_attachments <=
                            device.DescriptorLimits().maxFragmentCombinedOutputResources;
            }
            can_widen &= resources <= device.GetMaxPerStageResources();
        }
        if (!can_widen) {
            storage_buffer_stages = 0;
        }
    }
    DescriptorLayoutBuilder builder{device, storage_buffer_stages, num_color_attachments};
    for (size_t index = 0; index < infos.size(); ++index) {
        builder.Add(infos[index], stages.at(index));
    }
    return builder;
}

template <class StencilFace>
VkStencilOpState GetStencilFaceState(const StencilFace& face) {
    return {
        .failOp = MaxwellToVK::StencilOp(face.ActionStencilFail()),
        .passOp = MaxwellToVK::StencilOp(face.ActionDepthPass()),
        .depthFailOp = MaxwellToVK::StencilOp(face.ActionDepthFail()),
        .compareOp = MaxwellToVK::ComparisonOp(face.TestFunc()),
        .compareMask = 0,
        .writeMask = 0,
        .reference = 0,
    };
}

bool SupportsPrimitiveRestart(VkPrimitiveTopology topology) {
    static constexpr std::array unsupported_topologies{
        VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
        VK_PRIMITIVE_TOPOLOGY_PATCH_LIST,
        // VK_PRIMITIVE_TOPOLOGY_QUAD_LIST_EXT,
    };
    return std::ranges::find(unsupported_topologies, topology) == unsupported_topologies.end();
}

VkViewportSwizzleNV UnpackViewportSwizzle(u16 swizzle) {
    union Swizzle {
        u32 raw;
        BitField<0, 3, Maxwell::ViewportSwizzle> x;
        BitField<4, 3, Maxwell::ViewportSwizzle> y;
        BitField<8, 3, Maxwell::ViewportSwizzle> z;
        BitField<12, 3, Maxwell::ViewportSwizzle> w;
    };
    const Swizzle unpacked{swizzle};
    return VkViewportSwizzleNV{
        .x = MaxwellToVK::ViewportSwizzle(unpacked.x),
        .y = MaxwellToVK::ViewportSwizzle(unpacked.y),
        .z = MaxwellToVK::ViewportSwizzle(unpacked.z),
        .w = MaxwellToVK::ViewportSwizzle(unpacked.w),
    };
}

PixelFormat DecodeFormat(u8 encoded_format) {
    const auto format{static_cast<Tegra::RenderTargetFormat>(encoded_format)};
    if (format == Tegra::RenderTargetFormat::NONE) {
        return PixelFormat::Invalid;
    }
    return PixelFormatFromRenderTargetFormat(format);
}

RenderPassKey MakeRenderPassKey(const FixedPipelineState& state) {
    RenderPassKey key;
    std::ranges::transform(state.color_formats, key.color_formats.begin(), DecodeFormat);
    if (state.depth_enabled != 0) {
        const auto depth_format{static_cast<Tegra::DepthFormat>(state.depth_format.Value())};
        key.depth_format = PixelFormatFromDepthFormat(depth_format);
    } else {
        key.depth_format = PixelFormat::Invalid;
    }
    key.samples = MaxwellToVK::MsaaMode(state.msaa_mode);
    return key;
}

size_t NumAttachments(const FixedPipelineState& state) {
    size_t num{};
    for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
        const auto format{static_cast<Tegra::RenderTargetFormat>(state.color_formats[index])};
        if (format != Tegra::RenderTargetFormat::NONE) {
            num = index + 1;
        }
    }
    return num;
}

template <typename Spec>
bool Passes(const std::array<vk::ShaderModule, NUM_STAGES>& modules,
            const std::array<Shader::Info, NUM_STAGES>& stage_infos) {
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        if (!Spec::enabled_stages[stage] && modules[stage]) {
            return false;
        }
        const auto& info{stage_infos[stage]};
        if constexpr (!Spec::has_storage_buffers) {
            if (!info.storage_buffers_descriptors.empty()) {
                return false;
            }
        }
        if constexpr (!Spec::has_texture_buffers) {
            if (!info.texture_buffer_descriptors.empty()) {
                return false;
            }
        }
        if constexpr (!Spec::has_image_buffers) {
            if (!info.image_buffer_descriptors.empty()) {
                return false;
            }
        }
        if constexpr (!Spec::has_images) {
            if (!info.image_descriptors.empty()) {
                return false;
            }
        }
    }
    return true;
}

using ConfigureFuncPtr = void (*)(GraphicsPipeline*, bool);

template <typename Spec, typename... Specs>
ConfigureFuncPtr FindSpec(const std::array<vk::ShaderModule, NUM_STAGES>& modules,
                          const std::array<Shader::Info, NUM_STAGES>& stage_infos) {
    if constexpr (sizeof...(Specs) > 0) {
        if (!Passes<Spec>(modules, stage_infos)) {
            return FindSpec<Specs...>(modules, stage_infos);
        }
    }
    return GraphicsPipeline::MakeConfigureSpecFunc<Spec>();
}

struct SimpleVertexFragmentSpec {
    static constexpr std::array<bool, 5> enabled_stages{true, false, false, false, true};
    static constexpr bool has_storage_buffers = false;
    static constexpr bool has_texture_buffers = false;
    static constexpr bool has_image_buffers = false;
    static constexpr bool has_images = false;
};

struct SimpleVertexSpec {
    static constexpr std::array<bool, 5> enabled_stages{true, false, false, false, false};
    static constexpr bool has_storage_buffers = false;
    static constexpr bool has_texture_buffers = false;
    static constexpr bool has_image_buffers = false;
    static constexpr bool has_images = false;
};

struct SimpleStorageSpec {
    static constexpr std::array<bool, 5> enabled_stages{true, false, false, false, true};
    static constexpr bool has_storage_buffers = true;
    static constexpr bool has_texture_buffers = false;
    static constexpr bool has_image_buffers = false;
    static constexpr bool has_images = false;
};

struct SimpleImageSpec {
    static constexpr std::array<bool, 5> enabled_stages{true, false, false, false, true};
    static constexpr bool has_storage_buffers = false;
    static constexpr bool has_texture_buffers = false;
    static constexpr bool has_image_buffers = false;
    static constexpr bool has_images = true;
};

struct DefaultSpec {
    static constexpr std::array<bool, 5> enabled_stages{true, true, true, true, true};
    static constexpr bool has_storage_buffers = true;
    static constexpr bool has_texture_buffers = true;
    static constexpr bool has_image_buffers = true;
    static constexpr bool has_images = true;
};

ConfigureFuncPtr ConfigureFunc(const std::array<vk::ShaderModule, NUM_STAGES>& modules,
                               const std::array<Shader::Info, NUM_STAGES>& infos) {
    return FindSpec<SimpleVertexSpec, SimpleVertexFragmentSpec, SimpleStorageSpec, SimpleImageSpec,
                    DefaultSpec>(modules, infos);
}
} // Anonymous namespace

GraphicsPipeline::GraphicsPipeline(
    Scheduler& scheduler_, BufferCache& buffer_cache_, TextureCache& texture_cache_,
    vk::PipelineCache& pipeline_cache_, VideoCore::ShaderNotify* shader_notify,
    const Device& device_, DescriptorPool& descriptor_pool,
    GuestDescriptorQueue& guest_descriptor_queue_, Common::ThreadWorker* worker_thread,
    PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
    const GraphicsPipelineCacheKey& key_, std::array<vk::ShaderModule, NUM_STAGES> stages,
    const std::array<const Shader::Info*, NUM_STAGES>& infos, bool rasterizes_lines_)
    : key{key_}, device{device_}, texture_cache{texture_cache_}, buffer_cache{buffer_cache_},
      pipeline_cache(pipeline_cache_), scheduler{scheduler_},
      guest_descriptor_queue{guest_descriptor_queue_}, spv_modules{std::move(stages)},
      rasterizes_lines{rasterizes_lines_} {
    if (shader_notify) {
        shader_notify->MarkShaderBuilding();
    }
    for (size_t stage = 0; stage < NUM_STAGES; ++stage) {
        const Shader::Info* const info{infos[stage]};
        if (!info) {
            continue;
        }
        stage_infos[stage] = *info;
        enabled_uniform_buffer_masks[stage] = info->constant_buffer_mask;
        std::ranges::copy(info->constant_buffer_used_sizes, uniform_buffer_sizes[stage].begin());
        num_textures += Shader::NumDescriptors(info->texture_descriptors);
        num_descriptor_entries += NumDescriptorUpdateEntries(*info);
        num_image_elements += Shader::NumDescriptors(info->texture_buffer_descriptors);
        num_image_elements += Shader::NumDescriptors(info->image_buffer_descriptors);
        num_image_elements += Shader::NumDescriptors(info->texture_descriptors);
        num_image_elements += Shader::NumDescriptors(info->image_descriptors);
    }
    descriptor_views.resize(num_image_elements);
    descriptor_samplers.resize(num_textures);
    // Number of leading components the vertex shader may read from each attribute. Used to keep
    // vertex formats within their binding stride without discarding data the shader reads.
    const Shader::Info& vertex_info{stage_infos[0]};
    for (size_t index = 0; index < vertex_attribute_components.size(); ++index) {
        if (!vertex_info.loads.Generic(index)) {
            continue;
        }
        if (vertex_info.loads_indexed_attributes) {
            vertex_attribute_components[index] = 4;
            continue;
        }
        for (u8 component = 4; component != 0; --component) {
            if (vertex_info.loads.Generic(index, component - 1)) {
                vertex_attribute_components[index] = component;
                break;
            }
        }
    }
    auto func{[this, shader_notify, &render_pass_cache, &descriptor_pool, pipeline_statistics] {
        bool success{};
        try {
            DescriptorLayoutBuilder builder{MakeBuilder(
                device, stage_infos, static_cast<u32>(NumAttachments(key.state)))};
            uses_push_descriptor = builder.CanUsePushDescriptor();
            descriptor_set_layout = builder.CreateDescriptorSetLayout(uses_push_descriptor);
            if (!uses_push_descriptor) {
                descriptor_allocator = descriptor_pool.Allocator(*descriptor_set_layout, stage_infos);
            }
            const VkDescriptorSetLayout set_layout{*descriptor_set_layout};
            pipeline_layout = builder.CreatePipelineLayout(set_layout);
            descriptor_update_template =
                builder.CreateTemplate(set_layout, *pipeline_layout, uses_push_descriptor);

            const VkRenderPass render_pass{render_pass_cache.Get(MakeRenderPassKey(key.state))};
            Validate();
            MakePipeline(render_pass);
            if (pipeline_statistics) {
                pipeline_statistics->Collect(*pipeline);
            }

            success = true;
        } catch (const std::exception& exception) {
            LOG_ERROR(Render_Vulkan, "Graphics pipeline {:016x} build failed: {}", key.Hash(),
                      exception.what());
        }
        {
            std::scoped_lock lock{build_mutex};
            is_built.store(success, std::memory_order_release);
            build_complete.store(true, std::memory_order_release);
        }
        build_condvar.notify_all();
        if (shader_notify) {
            shader_notify->MarkShaderComplete();
        }
    }};
    if (worker_thread) {
        worker_thread->QueueWork(std::move(func));
    } else {
        func();
    }
    configure_func = ConfigureFunc(spv_modules, stage_infos);
}

// A pipeline can be shared by several keys, so the transition is keyed explicitly.
void GraphicsPipeline::AddTransition(GraphicsPipeline* transition,
                                     const GraphicsPipelineCacheKey& transition_key) {
    transition_keys.push_back(transition_key);
    transitions.push_back(transition);
}

template <typename Spec>
void GraphicsPipeline::ConfigureImpl(bool is_indexed) {
    auto& views = descriptor_views;
    auto& samplers = descriptor_samplers;
    size_t view_index{};
    size_t sampler_index{};

    texture_cache.SynchronizeGraphicsDescriptors();

    if (num_textures > INLINE_IMAGE_ELEMENTS) {
        if (!sampler_lookup) {
            sampler_lookup = std::make_unique<DrawDescriptorCache<VideoCommon::SamplerId>>();
        }
        sampler_lookup->Reset();
    }
    const bool memoize_samplers = sampler_lookup && sampler_lookup->Enabled();
    const auto lookup_sampler = [&](u32 index) {
        return texture_cache.GetGraphicsSamplerId(index);
    };

    buffer_cache.SetUniformBuffersState(enabled_uniform_buffer_masks, &uniform_buffer_sizes);

    const auto& regs{maxwell3d->regs};
    const bool via_header_index{regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding};
    const auto config_stage{[&](size_t stage) LAMBDA_FORCEINLINE {
        const Shader::Info& info{stage_infos[stage]};
        buffer_cache.UnbindGraphicsStorageBuffers(stage);
        if constexpr (Spec::has_storage_buffers) {
            size_t ssbo_index{};
            for (const auto& desc : info.storage_buffers_descriptors) {
                ASSERT(desc.count == 1);
                buffer_cache.BindGraphicsStorageBuffer(stage, ssbo_index, desc.cbuf_index,
                                                       desc.cbuf_offset, desc.is_written);
                ++ssbo_index;
            }
        }
        const auto& cbufs{maxwell3d->state.shader_stages[stage].const_buffers};
        DescriptorCbufReader<18> cbuf_reader;
        const auto bind_cbuf = [&](u32 bank, u32 count) {
            return cbuf_reader.Bind(*gpu_memory, bank, cbufs[bank].address, cbufs[bank].size, count);
        };
        const auto prepare_handle{[&](const auto& desc) {
            ASSERT(cbufs[desc.cbuf_index].enabled);
            const auto primary = bind_cbuf(desc.cbuf_index, desc.count);
            auto secondary = primary;
            bool has_secondary{};
            u32 secondary_offset{}, primary_shift{}, secondary_shift{};
            if constexpr (requires { desc.has_secondary; }) {
                if (desc.has_secondary) {
                    ASSERT(cbufs[desc.secondary_cbuf_index].enabled);
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
        const auto add_image{[&](const auto& desc, bool blacklist) LAMBDA_FORCEINLINE {
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
        if constexpr (Spec::has_texture_buffers) {
            for (const auto& desc : info.texture_buffer_descriptors) {
                add_image(desc, false);
            }
        }
        if constexpr (Spec::has_image_buffers) {
            for (const auto& desc : info.image_buffer_descriptors) {
                add_image(desc, false);
            }
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
        if constexpr (Spec::has_images) {
            for (const auto& desc : info.image_descriptors) {
                add_image(desc, desc.is_written);
            }
        }
    }};
    if constexpr (Spec::enabled_stages[0]) {
        config_stage(0);
    }
    if constexpr (Spec::enabled_stages[1]) {
        config_stage(1);
    }
    if constexpr (Spec::enabled_stages[2]) {
        config_stage(2);
    }
    if constexpr (Spec::enabled_stages[3]) {
        config_stage(3);
    }
    if constexpr (Spec::enabled_stages[4]) {
        config_stage(4);
    }
    ASSERT(view_index == views.size());
    ASSERT(sampler_index == samplers.size());
    texture_cache.FillGraphicsImageViews<Spec::has_images>(std::span(views.data(), views.size()));

    VideoCommon::ImageViewInOut* texture_buffer_it{views.data()};
    const auto bind_stage_info{[&](size_t stage) LAMBDA_FORCEINLINE {
        size_t index{};
        const auto add_buffer{[&](const auto& desc) {
            constexpr bool is_image = std::is_same_v<decltype(desc), const ImageBufferDescriptor&>;
            for (u32 i = 0; i < desc.count; ++i) {
                bool is_written{false};
                if constexpr (is_image) {
                    is_written = desc.is_written;
                }
                ImageView& image_view{texture_cache.GetImageView(texture_buffer_it->id)};
                buffer_cache.BindGraphicsTextureBuffer(stage, index, image_view.GpuAddr(),
                                                       image_view.BufferSize(), image_view.format,
                                                       is_written, is_image);
                ++index;
                ++texture_buffer_it;
            }
        }};
        buffer_cache.UnbindGraphicsTextureBuffers(stage);

        const Shader::Info& info{stage_infos[stage]};
        if constexpr (Spec::has_texture_buffers) {
            for (const auto& desc : info.texture_buffer_descriptors) {
                add_buffer(desc);
            }
        }
        if constexpr (Spec::has_image_buffers) {
            for (const auto& desc : info.image_buffer_descriptors) {
                add_buffer(desc);
            }
        }
        texture_buffer_it += Shader::NumDescriptors(info.texture_descriptors);
        if constexpr (Spec::has_images) {
            texture_buffer_it += Shader::NumDescriptors(info.image_descriptors);
        }
    }};
    if constexpr (Spec::enabled_stages[0]) {
        bind_stage_info(0);
    }
    if constexpr (Spec::enabled_stages[1]) {
        bind_stage_info(1);
    }
    if constexpr (Spec::enabled_stages[2]) {
        bind_stage_info(2);
    }
    if constexpr (Spec::enabled_stages[3]) {
        bind_stage_info(3);
    }
    if constexpr (Spec::enabled_stages[4]) {
        bind_stage_info(4);
    }

    buffer_cache.UpdateGraphicsBuffers(is_indexed);
    buffer_cache.BindHostGeometryBuffers(is_indexed);

    guest_descriptor_queue.Acquire(num_descriptor_entries);

    RescalingPushConstant rescaling;
    RenderAreaPushConstant render_area;
    const VideoCommon::SamplerId* samplers_it{samplers.data()};
    const VideoCommon::ImageViewInOut* views_it{views.data()};
    const auto prepare_stage{[&](size_t stage) LAMBDA_FORCEINLINE {
        buffer_cache.BindHostStageBuffers(stage);
        PushImageDescriptors(texture_cache, guest_descriptor_queue, stage_infos[stage], rescaling,
                             samplers_it, views_it);
        const auto& info{stage_infos[0]};
        if (info.uses_render_area) {
            render_area.uses_render_area = true;
            render_area.words = {static_cast<float>(regs.surface_clip.width),
                                 static_cast<float>(regs.surface_clip.height)};
        }
    }};
    if constexpr (Spec::enabled_stages[0]) {
        prepare_stage(0);
    }
    if constexpr (Spec::enabled_stages[1]) {
        prepare_stage(1);
    }
    if constexpr (Spec::enabled_stages[2]) {
        prepare_stage(2);
    }
    if constexpr (Spec::enabled_stages[3]) {
        prepare_stage(3);
    }
    if constexpr (Spec::enabled_stages[4]) {
        prepare_stage(4);
    }
    texture_cache.UpdateRenderTargets(false);
    texture_cache.CheckFeedbackLoop(views);
    ConfigureDraw(rescaling, render_area);
}

void GraphicsPipeline::ConfigureDraw(const RescalingPushConstant& rescaling,
                                     const RenderAreaPushConstant& render_area) {
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    u32 color_scratch_mask{};
    if (device.GetDriverID() == VK_DRIVER_ID_MOLTENVK) {
        const u32 duplicate_mask = framebuffer->DuplicateColorAttachmentMask();
        if (duplicate_mask != 0) {
            u32 color_write_mask{};
            const auto& regs = maxwell3d->regs;
            const Shader::Info& fragment_info = stage_infos[4];
            for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
                const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
                if ((mask.R || mask.G || mask.B || mask.A) &&
                    fragment_info.stores_frag_color[index]) {
                    color_write_mask |= 1U << index;
                }
            }
            // Avoid independent Metal tile copies of
            // the same image. Keep the render-pass shape, using scratch for masked slots.
            color_scratch_mask = framebuffer->ColorScratchMask(color_write_mask);
        }
    }
    scheduler.RequestRenderpass(framebuffer, color_scratch_mask);

    if (!build_complete.load(std::memory_order_acquire)) {
        // Completion includes failure, so a failed build cannot strand this worker.
        scheduler.Record([this](vk::CommandBuffer) {
            std::unique_lock lock{build_mutex};
            build_condvar.wait(lock, [this] { return build_complete.load(std::memory_order_acquire); });
        });
    }
    const bool is_rescaling{texture_cache.IsRescaling()};
    const bool update_rescaling{scheduler.UpdateRescaling(is_rescaling)};
    const bool bind_pipeline{scheduler.UpdateGraphicsPipeline(this)};
    const void* const descriptor_data{guest_descriptor_queue.UpdateData()};
    scheduler.Record([this, descriptor_data, bind_pipeline, rescaling_data = rescaling.Data(),
                      is_rescaling, update_rescaling,
                      uses_render_area = render_area.uses_render_area,
                      render_area_data = render_area.words](vk::CommandBuffer cmdbuf) {
        if (!IsBuilt()) {
            return;
        }
        if (bind_pipeline) {
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
        }
        cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_ALL_GRAPHICS,
                             RESCALING_LAYOUT_WORDS_OFFSET, sizeof(rescaling_data),
                             rescaling_data.data());
        if (update_rescaling || bind_pipeline) {
            const f32 config_down_factor{videoSettings.resolution_info.down_factor};
            const f32 scale_down_factor{is_rescaling ? config_down_factor : 1.0f};
            cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_ALL_GRAPHICS,
                                 RESCALING_LAYOUT_DOWN_FACTOR_OFFSET, sizeof(scale_down_factor),
                                 &scale_down_factor);
        }
        if (uses_render_area) {
            cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_ALL_GRAPHICS,
                                 RENDERAREA_LAYOUT_OFFSET, sizeof(render_area_data),
                                 &render_area_data);
        }
        if (!descriptor_set_layout) {
            return;
        }
        if (uses_push_descriptor) {
            cmdbuf.PushDescriptorSetWithTemplateKHR(*descriptor_update_template, *pipeline_layout,
                                                    0, descriptor_data);
        } else {
            const VkDescriptorSet descriptor_set{descriptor_allocator.Commit()};
            const vk::Device& dev{device.GetLogical()};
            dev.UpdateDescriptorSet(descriptor_set, *descriptor_update_template, descriptor_data);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline_layout, 0,
                                      descriptor_set, nullptr);
        }
    });
    if (bind_pipeline && device.GetDriverID() == VK_DRIVER_ID_MOLTENVK &&
        key.state.extended_dynamic_state_3_blend != 0) {
        std::array<VkBool32, Maxwell::NumRenderTargets> setup_enables{};
        const auto& regs = maxwell3d->regs;
        for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
            const bool integer_format = VideoCore::Surface::IsPixelFormatInteger(
                DecodeFormat(key.state.color_formats[index]));
            setup_enables[index] =
                regs.blend.enable[index] != 0 && !integer_format ? VK_TRUE : VK_FALSE;
        }
        scheduler.Record([setup_enables](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEnableEXT(0, setup_enables);
        });
    }
}

void GraphicsPipeline::MakePipeline(VkRenderPass render_pass) {
    FixedPipelineState::DynamicState dynamic{};
    if (!key.state.extended_dynamic_state) {
        dynamic = key.state.dynamic_state;
    } else {
        dynamic.raw1 = key.state.dynamic_state.raw1;
    }
    static_vector<VkVertexInputBindingDescription, 32> vertex_bindings;
    static_vector<VkVertexInputBindingDivisorDescriptionEXT, 32> vertex_binding_divisors;
    static_vector<VkVertexInputAttributeDescription, 32> vertex_attributes;
    if (!key.state.dynamic_vertex_input) {
        const size_t num_vertex_arrays = std::min(
            Maxwell::NumVertexArrays, static_cast<size_t>(device.GetMaxVertexInputBindings()));
        for (size_t index = 0; index < num_vertex_arrays; ++index) {
            const bool instanced = key.state.binding_divisors[index] != 0;
            const auto rate =
                instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            vertex_bindings.push_back({
                .binding = static_cast<u32>(index),
                .stride = key.state.vertex_strides[index],
                .inputRate = rate,
            });
            if (instanced) {
                vertex_binding_divisors.push_back({
                    .binding = static_cast<u32>(index),
                    .divisor = key.state.binding_divisors[index],
                });
            }
        }
        for (size_t index = 0; index < key.state.attributes.size(); ++index) {
            const auto& attribute = key.state.attributes[index];
            if (!attribute.enabled || !stage_infos[0].loads.Generic(index)) {
                continue;
            }
            const VkFormat format =
                key.state.vertex_stride_workaround != 0 && attribute.buffer < num_vertex_arrays
                    ? MaxwellToVK::VertexFormat(device, attribute.Type(), attribute.Size(),
                                                attribute.offset,
                                                key.state.vertex_strides[attribute.buffer],
                                                vertex_attribute_components[index])
                    : MaxwellToVK::VertexFormat(device, attribute.Type(), attribute.Size());
            vertex_attributes.push_back({
                .location = static_cast<u32>(index),
                .binding = attribute.buffer,
                .format = format,
                .offset = attribute.offset,
            });
        }
    }
    ASSERT(vertex_attributes.size() <= device.GetMaxVertexInputAttributes());

    VkPipelineVertexInputStateCreateInfo vertex_input_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = static_cast<u32>(vertex_bindings.size()),
        .pVertexBindingDescriptions = vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(vertex_attributes.size()),
        .pVertexAttributeDescriptions = vertex_attributes.data(),
    };
    const VkPipelineVertexInputDivisorStateCreateInfoEXT input_divisor_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .vertexBindingDivisorCount = static_cast<u32>(vertex_binding_divisors.size()),
        .pVertexBindingDivisors = vertex_binding_divisors.data(),
    };
    if (!vertex_binding_divisors.empty()) {
        vertex_input_ci.pNext = &input_divisor_ci;
    }
    const bool has_tess_stages = spv_modules[1] || spv_modules[2];
    auto input_assembly_topology = MaxwellToVK::PrimitiveTopology(device, key.state.topology);
    if (input_assembly_topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {
        if (!has_tess_stages) {
            LOG_WARNING(Render_Vulkan, "Patch topology used without tessellation, using points");
            input_assembly_topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        }
    } else {
        if (has_tess_stages) {
            // The Vulkan spec requires patch list IA topology be used with tessellation
            // shader stages. Forcing it fixes a crash on some drivers
            LOG_WARNING(Render_Vulkan,
                        "Patch topology not used with tessellation, using patch list");
            input_assembly_topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        }
    }
    const VkPipelineInputAssemblyStateCreateInfo input_assembly_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = input_assembly_topology,
        .primitiveRestartEnable =
            dynamic.primitive_restart_enable != 0 &&
                    ((input_assembly_topology != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
                      device.IsTopologyListPrimitiveRestartSupported()) ||
                     SupportsPrimitiveRestart(input_assembly_topology) ||
                     (input_assembly_topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
                      device.IsPatchListPrimitiveRestartSupported()))
                ? VK_TRUE
                : VK_FALSE,
    };
    const VkPipelineTessellationStateCreateInfo tessellation_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .patchControlPoints = key.state.patch_control_points_minus_one.Value() + 1,
    };
    std::array<VkViewportSwizzleNV, Maxwell::NumViewports> swizzles;
    std::ranges::transform(key.state.viewport_swizzles, swizzles.begin(), UnpackViewportSwizzle);
    VkPipelineViewportSwizzleStateCreateInfoNV swizzle_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SWIZZLE_STATE_CREATE_INFO_NV,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = Maxwell::NumViewports,
        .pViewportSwizzles = swizzles.data(),
    };
    VkPipelineViewportDepthClipControlCreateInfoEXT ndc_info{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
        .pNext = nullptr,
        .negativeOneToOne = key.state.ndc_minus_one_to_one.Value() != 0 ? VK_TRUE : VK_FALSE,
    };
    const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
    VkPipelineViewportStateCreateInfo viewport_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = num_viewports,
        .pViewports = nullptr,
        .scissorCount = num_viewports,
        .pScissors = nullptr,
    };
    if (device.IsNvViewportSwizzleSupported()) {
        swizzle_ci.pNext = std::exchange(viewport_ci.pNext, &swizzle_ci);
    }
    if (device.IsExtDepthClipControlSupported()) {
        ndc_info.pNext = std::exchange(viewport_ci.pNext, &ndc_info);
    }
    VkPipelineRasterizationStateCreateInfo rasterization_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable =
            static_cast<VkBool32>(dynamic.depth_clamp_disabled == 0 ? VK_TRUE : VK_FALSE),
        .rasterizerDiscardEnable =
            static_cast<VkBool32>(dynamic.rasterize_enable == 0 ? VK_TRUE : VK_FALSE),
        .polygonMode =
            MaxwellToVK::PolygonMode(FixedPipelineState::UnpackPolygonMode(key.state.polygon_mode)),
        .cullMode = static_cast<VkCullModeFlags>(
            dynamic.cull_enable ? MaxwellToVK::CullFace(dynamic.CullFace()) : VK_CULL_MODE_NONE),
        .frontFace = MaxwellToVK::FrontFace(dynamic.FrontFace()),
        .depthBiasEnable = (dynamic.depth_bias_enable != 0 ? VK_TRUE : VK_FALSE),
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f,
    };
    const bool smooth_lines_supported =
        device.IsExtLineRasterizationSupported() && device.SupportsSmoothLines();
    const bool rectangular_lines_supported =
        device.IsExtLineRasterizationSupported() && device.SupportsRectangularLines();
    // As in OpenGL, line smoothing is ignored when rendering to a multisampled target.
    const bool is_multisampled =
        MaxwellToVK::MsaaMode(key.state.msaa_mode) != VK_SAMPLE_COUNT_1_BIT;
    const VkLineRasterizationModeEXT line_rasterization_mode =
        key.state.smooth_lines != 0 && smooth_lines_supported && !is_multisampled
            ? VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT
            : rectangular_lines_supported ? VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT
                                          : VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT;
    const bool use_line_state =
        rasterizes_lines && device.IsExtLineRasterizationSupported();
    const bool smooth_line_rasterization =
        use_line_state &&
        line_rasterization_mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT;
    VkPipelineRasterizationLineStateCreateInfoEXT line_state{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .lineRasterizationMode = line_rasterization_mode,
        .stippledLineEnable = VK_FALSE, // TODO
        .lineStippleFactor = 0,
        .lineStipplePattern = 0,
    };
    VkPipelineRasterizationConservativeStateCreateInfoEXT conservative_raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .conservativeRasterizationMode = key.state.conservative_raster_enable != 0
                                             ? VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT
                                             : VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT,
        .extraPrimitiveOverestimationSize = 0.0f,
    };
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking_vertex{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .provokingVertexMode = key.state.provoking_vertex_last != 0
                                   ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                                   : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT,
    };
    if (use_line_state) {
        line_state.pNext = std::exchange(rasterization_ci.pNext, &line_state);
    }
    if (device.IsExtConservativeRasterizationSupported()) {
        conservative_raster.pNext = std::exchange(rasterization_ci.pNext, &conservative_raster);
    }
    if (device.IsExtProvokingVertexSupported()) {
        provoking_vertex.pNext = std::exchange(rasterization_ci.pNext, &provoking_vertex);
    }

    const bool supports_alpha_output = stage_infos[NUM_STAGES - 1].stores_frag_color[0];
    const VkPipelineMultisampleStateCreateInfo multisample_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = MaxwellToVK::MsaaMode(key.state.msaa_mode),
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 0.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = !smooth_line_rasterization && supports_alpha_output &&
                                         key.state.alpha_to_coverage_enabled != 0
                                     ? VK_TRUE
                                     : VK_FALSE,
        .alphaToOneEnable = !smooth_line_rasterization && supports_alpha_output &&
                                   device.SupportsAlphaToOne() &&
                                   key.state.alpha_to_one_enabled != 0
                               ? VK_TRUE
                               : VK_FALSE,
    };
    const VkPipelineDepthStencilStateCreateInfo depth_stencil_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthTestEnable = dynamic.depth_test_enable,
        .depthWriteEnable = dynamic.depth_write_enable,
        .depthCompareOp = dynamic.depth_test_enable
                              ? MaxwellToVK::ComparisonOp(dynamic.DepthTestFunc())
                              : VK_COMPARE_OP_ALWAYS,
        .depthBoundsTestEnable = dynamic.depth_bounds_enable && device.IsDepthBoundsSupported(),
        .stencilTestEnable = dynamic.stencil_enable,
        .front = GetStencilFaceState(dynamic.front),
        .back = GetStencilFaceState(dynamic.back),
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 0.0f,
    };
    if (dynamic.depth_bounds_enable && !device.IsDepthBoundsSupported()) {
        LOG_WARNING(Render_Vulkan, "Depth bounds is enabled but not supported");
    }
    static_vector<VkPipelineColorBlendAttachmentState, Maxwell::NumRenderTargets> cb_attachments;
    const size_t num_attachments{NumAttachments(key.state)};
    for (size_t index = 0; index < num_attachments; ++index) {
        static constexpr std::array mask_table{
            VK_COLOR_COMPONENT_R_BIT,
            VK_COLOR_COMPONENT_G_BIT,
            VK_COLOR_COMPONENT_B_BIT,
            VK_COLOR_COMPONENT_A_BIT,
        };
        const auto& blend{key.state.attachments[index]};
        const std::array mask{blend.Mask()};
        VkColorComponentFlags write_mask{};
        for (size_t i = 0; i < mask_table.size(); ++i) {
            write_mask |= mask[i] ? mask_table[i] : 0;
        }
        // Blending is not defined for integer formats.
        const bool disable_integer_blend =
            VideoCore::Surface::IsPixelFormatInteger(DecodeFormat(key.state.color_formats[index]));
        cb_attachments.push_back({
            .blendEnable = blend.enable != 0 && !disable_integer_blend,
            .srcColorBlendFactor = MaxwellToVK::BlendFactor(blend.SourceRGBFactor()),
            .dstColorBlendFactor = MaxwellToVK::BlendFactor(blend.DestRGBFactor()),
            .colorBlendOp = MaxwellToVK::BlendEquation(blend.EquationRGB()),
            .srcAlphaBlendFactor = MaxwellToVK::BlendFactor(blend.SourceAlphaFactor()),
            .dstAlphaBlendFactor = MaxwellToVK::BlendFactor(blend.DestAlphaFactor()),
            .alphaBlendOp = MaxwellToVK::BlendEquation(blend.EquationAlpha()),
            .colorWriteMask = write_mask,
        });
    }
    const VkPipelineColorBlendStateCreateInfo color_blend_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = dynamic.logic_op_enable != 0,
        .logicOp = static_cast<VkLogicOp>(dynamic.logic_op.Value()),
        .attachmentCount = static_cast<u32>(cb_attachments.size()),
        .pAttachments = cb_attachments.data(),
        .blendConstants = {},
    };
    static_vector<VkDynamicState, 28> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS,         VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS,       VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_LINE_WIDTH,
    };
    if (key.state.extended_dynamic_state) {
        static constexpr std::array extended{
            VK_DYNAMIC_STATE_CULL_MODE_EXT,
            VK_DYNAMIC_STATE_FRONT_FACE_EXT,
            VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT,
            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT,
            VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT,
            VK_DYNAMIC_STATE_STENCIL_OP_EXT,
        };
        if (key.state.dynamic_vertex_input) {
            dynamic_states.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);
        }
        dynamic_states.insert(dynamic_states.end(), extended.begin(), extended.end());
        if (key.state.extended_dynamic_state_2) {
            static constexpr std::array extended2{
                VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE_EXT,
                VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE_EXT,
                VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE_EXT,
            };
            dynamic_states.insert(dynamic_states.end(), extended2.begin(), extended2.end());
        }
        if (key.state.extended_dynamic_state_2_extra) {
            dynamic_states.push_back(VK_DYNAMIC_STATE_LOGIC_OP_EXT);
        }
        if (key.state.extended_dynamic_state_3_blend) {
            static constexpr std::array extended3{
                VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT,
                VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
                VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT,
            };
            dynamic_states.insert(dynamic_states.end(), extended3.begin(), extended3.end());
        }
        if (key.state.extended_dynamic_state_3_enables) {
            static constexpr std::array extended3{
                VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT,
                VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT,
            };
            dynamic_states.insert(dynamic_states.end(), extended3.begin(), extended3.end());
        }
    }
    if (!device.CanUseDepthBoundsDynamicState()) {
        dynamic_states.erase(
            std::remove_if(dynamic_states.begin(), dynamic_states.end(), [](VkDynamicState state) {
                return state == VK_DYNAMIC_STATE_DEPTH_BOUNDS ||
                       state == VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE_EXT;
            }),
            dynamic_states.end());
    }
    const VkPipelineDynamicStateCreateInfo dynamic_state_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    [[maybe_unused]] const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup_size_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .requiredSubgroupSize = GuestWarpSize,
    };
    static_vector<VkPipelineShaderStageCreateInfo, 5> shader_stages;
    for (size_t stage = 0; stage < Maxwell::MaxShaderStage; ++stage) {
        if (!spv_modules[stage]) {
            continue;
        }
        [[maybe_unused]] auto& stage_ci =
            shader_stages.emplace_back(VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = MaxwellToVK::ShaderStage(Shader::StageFromIndex(stage)),
                .module = *spv_modules[stage],
                .pName = "main",
                .pSpecializationInfo = nullptr,
            });
        /*
        if (program[stage]->entries.uses_warps && device.IsGuestWarpSizeSupported(stage_ci.stage)) {
            stage_ci.pNext = &subgroup_size_ci;
        }
        */
    }
    VkPipelineCreateFlags flags{};
    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    }
    pipeline = device.GetLogical().CreateGraphicsPipeline(
        {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = flags,
            .stageCount = static_cast<u32>(shader_stages.size()),
            .pStages = shader_stages.data(),
            .pVertexInputState = &vertex_input_ci,
            .pInputAssemblyState = &input_assembly_ci,
            .pTessellationState = &tessellation_ci,
            .pViewportState = &viewport_ci,
            .pRasterizationState = &rasterization_ci,
            .pMultisampleState = &multisample_ci,
            .pDepthStencilState = &depth_stencil_ci,
            .pColorBlendState = &color_blend_ci,
            .pDynamicState = &dynamic_state_ci,
            .layout = *pipeline_layout,
            .renderPass = render_pass,
            .subpass = 0,
            .basePipelineHandle = nullptr,
            .basePipelineIndex = 0,
        },
        *pipeline_cache);
}

void GraphicsPipeline::Validate() {
    size_t num_images{};
    for (const auto& info : stage_infos) {
        num_images += Shader::NumDescriptors(info.texture_buffer_descriptors);
        num_images += Shader::NumDescriptors(info.image_buffer_descriptors);
        num_images += Shader::NumDescriptors(info.texture_descriptors);
        num_images += Shader::NumDescriptors(info.image_descriptors);
    }
    ASSERT(num_images == num_image_elements);
}

} // namespace Vulkan
