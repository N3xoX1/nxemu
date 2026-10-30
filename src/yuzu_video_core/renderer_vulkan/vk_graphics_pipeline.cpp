// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_video_core/shader_profile_runtime.h"
#include <algorithm>
#include "yuzu_video_core/shader_profile_metrics.h"
#include <bit>
#include <cstring>
#include <span>

#include <boost/container/static_vector.hpp>

#include "yuzu_video_core/renderer_vulkan/pipeline_helper.h"

#include "yuzu_common/alignment.h"
#include "yuzu_common/bit_field.h"
#include "yuzu_common/div_ceil.h"
#include "yuzu_video_core/renderer_vulkan/maxwell_to_vk.h"
#include "yuzu_video_core/renderer_vulkan/pipeline_statistics.h"
#include "yuzu_video_core/renderer_vulkan/vk_buffer_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "yuzu_video_core/renderer_vulkan/vk_rasterizer.h"
#include "yuzu_video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_staging_buffer_pool.h"
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
namespace Timing = VideoCommon::ShaderProfile::Timing;
namespace {
using boost::container::static_vector;
using Shader::ImageBufferDescriptor;
using Shader::Backend::SPIRV::RENDERAREA_LAYOUT_OFFSET;
using Shader::Backend::SPIRV::RESCALING_LAYOUT_ATTACHMENT_READS_OFFSET;
using Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET;
using Shader::Backend::SPIRV::RESCALING_LAYOUT_WORDS_OFFSET;
using Tegra::Texture::TexturePair;
using VideoCore::Surface::PixelFormat;
using VideoCore::Surface::PixelFormatFromDepthFormat;
using VideoCore::Surface::PixelFormatFromRenderTargetFormat;

constexpr size_t NUM_STAGES = Maxwell::MaxShaderStage;
constexpr size_t INLINE_IMAGE_ELEMENTS = 64;

constexpr std::array<VkPipelineStageFlagBits, NUM_STAGES> PIPELINE_STAGE_FLAGS{
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
    VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT,
    VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT,
    VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT,
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
};

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

RenderPassKey MakeRenderPassKey(const Device& device, const FixedPipelineState& state) {
    RenderPassKey key;
    std::ranges::transform(state.color_formats, key.color_formats.begin(), DecodeFormat);
    if (state.depth_enabled != 0) {
        const auto depth_format{static_cast<Tegra::DepthFormat>(state.depth_format.Value())};
        key.depth_format = PixelFormatFromDepthFormat(depth_format);
    } else {
        key.depth_format = PixelFormat::Invalid;
    }
    key.samples = device.SupportedSampleCount(MaxwellToVK::MsaaMode(state.msaa_mode));
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

VtgScratchAllocator::VtgScratchAllocator(const Device& device, Scheduler& scheduler_,
                                         StagingBufferPool& staging_pool_)
    : scheduler{scheduler_}, staging_pool{staging_pool_},
      alignment{std::max<VkDeviceSize>(device.GetStorageBufferAlignment(), sizeof(u32))} {}

std::pair<VkBuffer, VkDeviceSize> VtgScratchAllocator::Allocate(VkDeviceSize size) {
    static constexpr VkDeviceSize CHUNK_SIZE = 4ULL << 20;
    size = Common::AlignUp(size, alignment);
    const u64 current_tick{scheduler.CurrentTick()};
    if (tick != current_tick) {
        // A draw may be recorded on the tick following the one its stages ran on, the chunks are
        // kept from being reused until that tick completes
        for (StagingBufferRef& chunk : chunks) {
            staging_pool.FreeDeferred(chunk);
        }
        chunks.clear();
        buffer = VK_NULL_HANDLE;
        tick = current_tick;
    }
    if (buffer == VK_NULL_HANDLE || used + size > capacity) {
        capacity = std::max(CHUNK_SIZE, size);
        const StagingBufferRef ref{staging_pool.Request(capacity, MemoryUsage::DeviceLocal, true)};
        chunks.push_back(ref);
        buffer = ref.buffer;
        base = ref.offset;
        used = 0;
    }
    const VkDeviceSize offset{base + used};
    used += size;
    return {buffer, offset};
}

struct GraphicsPipeline::VtgState {
    struct Stage {
        /// Shader information including the storage buffers added to the stage
        Shader::Info info;
        /// Shader information of the resources bound by the guest
        Shader::Info guest_info;
        size_t num_descriptor_entries{};
        vk::ShaderModule module;
        vk::DescriptorSetLayout descriptor_set_layout;
        DescriptorAllocator descriptor_allocator;
        vk::PipelineLayout pipeline_layout;
        vk::DescriptorUpdateTemplate descriptor_update_template;
        vk::Pipeline pipeline;
    };
    struct Buffer {
        VkBuffer buffer;
        VkDeviceSize offset;
        VkDeviceSize size;
    };

    std::array<Stage, VtgPipelineInfo::NUM_COMPUTE_STAGES> stages;
    Shader::VtgVertexBindings vertex_bindings;
    Shader::VtgGeometryBindings geometry_bindings;
    Shader::VtgTopology topology{};
    u32 vertex_stride{};
    u32 geometry_stride{};
    VtgScratchAllocator* scratch{};
    StagingBufferPool* staging_pool{};

    std::array<u32, NUM_STAGES> uniform_masks{};
    VideoCommon::UniformBufferSizes uniform_sizes{};

    u32 instance_count{};
    VkBuffer index_buffer{};
    VkDeviceSize index_offset{};
    u32 index_count{};
};

struct GraphicsPipeline::XfbState {
    VtgState::Stage stage;
    Shader::VtgVertexBindings bindings;
    Shader::VtgTransformFeedback layout;
    VtgScratchAllocator* scratch{};
    StagingBufferPool* staging_pool{};
    u32 instance_count{};
};

GraphicsPipeline::~GraphicsPipeline() = default;

void GraphicsPipeline::SetVtgInstanceCount(u32 instance_count) noexcept {
    if (vtg) {
        vtg->instance_count = instance_count;
    }
    if (xfb) {
        xfb->instance_count = instance_count;
    }
}

std::tuple<VkBuffer, VkDeviceSize, u32> GraphicsPipeline::VtgDraw() const noexcept {
    return {vtg->index_buffer, vtg->index_offset, vtg->index_count};
}

GraphicsPipeline::GraphicsPipeline(
    Scheduler& scheduler_, BufferCache& buffer_cache_, TextureCache& texture_cache_,
    vk::PipelineCache& pipeline_cache_, VideoCore::ShaderNotify* shader_notify,
    const Device& device_, DescriptorPool& descriptor_pool,
    GuestDescriptorQueue& guest_descriptor_queue_, NativePipelineBuildQueue* worker_thread,
    PipelineStatistics* pipeline_statistics, RenderPassCache& render_pass_cache,
    const GraphicsPipelineCacheKey& key_, std::array<vk::ShaderModule, NUM_STAGES> stages,
    const std::array<const Shader::Info*, NUM_STAGES>& infos, bool rasterizes_lines_,
    std::unique_ptr<VtgPipelineInfo> vtg_info, bool urgent_build)
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
        const auto is_written = [](const auto& desc) { return desc.is_written; };
        if (std::ranges::any_of(info->image_descriptors, is_written) ||
            std::ranges::any_of(info->image_buffer_descriptors, is_written)) {
            image_write_stages |= PIPELINE_STAGE_FLAGS[stage];
        }
        enabled_uniform_buffer_masks[stage] = info->constant_buffer_mask;
        std::ranges::copy(info->constant_buffer_used_sizes, uniform_buffer_sizes[stage].begin());
        num_textures += NumCombinedSamplers(*info);
        uses_texture_table |= info->runtime_sampler_count != 0 ||
                              NumTableSetCombinedSamplers(*info) != 0;
        num_descriptor_entries += NumDescriptorUpdateEntries(device, *info);
        num_image_elements += Shader::NumDescriptors(info->texture_buffer_descriptors);
        num_image_elements += Shader::NumDescriptors(info->image_buffer_descriptors);
        num_image_elements += Shader::NumDescriptors(info->texture_descriptors);
        num_image_elements += Shader::NumDescriptors(info->image_descriptors);
    }
    if (stage_infos[NUM_STAGES - 1].input_attachment_mask != 0) {
        for (const auto& desc : stage_infos[NUM_STAGES - 1].texture_descriptors) {
            if (desc.pixel_local_read != Shader::PixelLocalRead::None) {
                attachment_read_kinds.push_back(desc.pixel_local_read);
            }
        }
    }
    if (vtg_info && vtg_info->is_transform_feedback) {
        // The compute stage reuses the guest resources bound for the vertex stage
        const Shader::Info& info{vtg_info->infos[VtgPipelineInfo::VERTEX]};
        const Shader::Info& vertex{stage_infos[0]};
        const auto same_count{[](const auto& lhs, const auto& rhs) {
            return Shader::NumDescriptors(lhs) == Shader::NumDescriptors(rhs);
        }};
        if (vtg_info->vertex_bindings.base == vertex.storage_buffers_descriptors.size() &&
            same_count(info.texture_buffer_descriptors, vertex.texture_buffer_descriptors) &&
            same_count(info.image_buffer_descriptors, vertex.image_buffer_descriptors) &&
            same_count(info.texture_descriptors, vertex.texture_descriptors) &&
            same_count(info.image_descriptors, vertex.image_descriptors) &&
            info.constant_buffer_mask == vertex.constant_buffer_mask) {
            xfb = std::make_unique<XfbState>();
            xfb->stage.info = info;
            xfb->stage.num_descriptor_entries = NumDescriptorUpdateEntries(device, info);
            xfb->stage.guest_info = info;
            xfb->stage.guest_info.storage_buffers_descriptors.resize(vtg_info->vertex_bindings.base);
            xfb->stage.module = std::move(vtg_info->modules[VtgPipelineInfo::VERTEX]);
            xfb->bindings = vtg_info->vertex_bindings;
            xfb->layout = vtg_info->transform_feedback;
            xfb->scratch = vtg_info->scratch;
            xfb->staging_pool = vtg_info->staging_pool;
        } else {
            LOG_ERROR(Render_Vulkan, "Transform feedback stage of pipeline {:016x} does not use the "
                                     "resources of its vertex stage", key.Hash());
        }
    } else if (vtg_info) {
        static constexpr std::array<size_t, VtgPipelineInfo::NUM_COMPUTE_STAGES> compute_stages{
            0, 3};
        const std::array<u32, VtgPipelineInfo::NUM_COMPUTE_STAGES> guest_storage_buffers{
            vtg_info->vertex_bindings.base, vtg_info->geometry_bindings.base};
        vtg = std::make_unique<VtgState>();
        vtg->uniform_masks = enabled_uniform_buffer_masks;
        vtg->uniform_sizes = uniform_buffer_sizes;
        for (size_t index = 0; index < vtg->stages.size(); ++index) {
            VtgState::Stage& stage{vtg->stages[index]};
            stage.info = vtg_info->infos[index];
            stage.num_descriptor_entries = NumDescriptorUpdateEntries(device, stage.info);
            stage.guest_info = stage.info;
            stage.guest_info.storage_buffers_descriptors.resize(guest_storage_buffers[index]);
            // The compute stages retain guest images absent from the generated vertex shader.
            num_textures += NumCombinedSamplers(stage.guest_info);
            num_image_elements += Shader::NumDescriptors(stage.guest_info.texture_buffer_descriptors);
            num_image_elements += Shader::NumDescriptors(stage.guest_info.image_buffer_descriptors);
            num_image_elements += Shader::NumDescriptors(stage.guest_info.texture_descriptors);
            num_image_elements += Shader::NumDescriptors(stage.guest_info.image_descriptors);
            stage.module = std::move(vtg_info->modules[index]);
            vtg->uniform_masks[compute_stages[index]] = stage.info.constant_buffer_mask;
            std::ranges::copy(stage.info.constant_buffer_used_sizes,
                              vtg->uniform_sizes[compute_stages[index]].begin());
        }
        vtg->vertex_bindings = vtg_info->vertex_bindings;
        vtg->geometry_bindings = vtg_info->geometry_bindings;
        vtg->topology = vtg_info->topology;
        vtg->vertex_stride = vtg_info->vertex_stride;
        vtg->geometry_stride = vtg_info->geometry_stride;
        vtg->scratch = vtg_info->scratch;
        vtg->staging_pool = vtg_info->staging_pool;
    }
    descriptor_views.resize(num_image_elements);
    descriptor_samplers.resize(num_textures);
    // Number of leading components the vertex shader may read from each attribute. Used to keep
    // vertex formats within their binding stride without discarding data the shader reads.
    const Shader::Info& vertex_info{stage_infos[0]};
    if (!vtg && key.state.dynamic_vertex_input == 0 &&
        (device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes ||
         device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays)) {
        vertex_input_mapping = MakeVertexInputMapping(key.state, vertex_info.loads);
    }
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
    auto func{[this, shader_notify, &render_pass_cache, &descriptor_pool, pipeline_statistics, runtime_event=VideoCommon::ShaderProfile::Runtime::current] {
        VideoCommon::ShaderProfile::Runtime::Span runtime_timer(runtime_event,VideoCommon::ShaderProfile::Runtime::Work::Native);
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
            if (builder.HasTextureTable()) {
                const auto table_bindings{builder.TextureTableBindings()};
                texture_table_bindings.assign(table_bindings.begin(), table_bindings.end());
                DescriptorBankInfo bank{};
                for (const auto& binding : table_bindings) {
                    switch (binding.descriptorType) {
                    case VK_DESCRIPTOR_TYPE_SAMPLER:
                        bank.samplers += binding.descriptorCount;
                        break;
                    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                        bank.textures += binding.descriptorCount;
                        break;
                    default:
                        bank.sampled_images += binding.descriptorCount;
                        break;
                    }
                }
                bank.score = static_cast<s32>(bank.textures + bank.sampled_images + bank.samplers);
                texture_table_set_layout = builder.CreateTextureTableSetLayout();
                texture_table_allocator =
                    descriptor_pool.Allocator(*texture_table_set_layout, bank);
            }
            pipeline_layout = builder.CreatePipelineLayout(set_layout, *texture_table_set_layout);
            descriptor_update_template =
                builder.CreateTemplate(set_layout, *pipeline_layout, uses_push_descriptor);
            if (!uses_push_descriptor && device.GetDriverID() == VK_DRIVER_ID_MOLTENVK) {
                for (const auto& entry : builder.TemplateEntries()) {
                    if (DescriptorLayoutBuilder::IsLargeImageArray(entry)) {
                        large_image_entries.push_back(entry);
                    }
                }
                if (!large_image_entries.empty()) {
                    small_descriptor_update_template =
                        builder.CreateTemplate(set_layout, *pipeline_layout, false, true);
                }
            }

            const auto build_compute_stage{[&](VtgState::Stage& stage) {
                DescriptorLayoutBuilder stage_builder{device};
                stage_builder.Add(stage.info, VK_SHADER_STAGE_COMPUTE_BIT);
                stage.descriptor_set_layout = stage_builder.CreateDescriptorSetLayout(false);
                stage.pipeline_layout =
                    stage_builder.CreatePipelineLayout(*stage.descriptor_set_layout);
                stage.descriptor_update_template = stage_builder.CreateTemplate(
                    *stage.descriptor_set_layout, *stage.pipeline_layout, false);
                stage.descriptor_allocator =
                    descriptor_pool.Allocator(*stage.descriptor_set_layout, stage.info);
                stage.pipeline = Timing::Measure(Timing::current, Timing::Phase::NativeCompute, [&] { return device.GetLogical().CreateComputePipeline(
                    {
                        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                        .pNext = nullptr,
                        .flags = 0,
                        .stage{
                            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                            .pNext = nullptr,
                            .flags = 0,
                            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                            .module = *stage.module,
                            .pName = "main",
                            .pSpecializationInfo = nullptr,
                        },
                        .layout = *stage.pipeline_layout,
                        .basePipelineHandle = 0,
                        .basePipelineIndex = 0,
                    },
                    *pipeline_cache); });
            }};
            if (xfb) {
                build_compute_stage(xfb->stage);
            }
            if (vtg) {
                for (VtgState::Stage& stage : vtg->stages) {
                    build_compute_stage(stage);
                }
            }
            const VkRenderPass render_pass{render_pass_cache.Get(MakeRenderPassKey(device, key.state))};
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
        VideoCommon::ShaderProfile::Runtime::NativeResult(runtime_event,success);
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
        build_queue = worker_thread;
        queued_build = worker_thread->QueueWork(std::move(func), urgent_build);
    } else {
        func();
    }
    if (vtg) {
        configure_func = [](GraphicsPipeline* pl, bool is_indexed) { pl->ConfigureVtg(is_indexed); };
    } else {
        configure_func = ConfigureFunc(spv_modules, stage_infos);
    }
}

void GraphicsPipeline::Configure(bool is_indexed) {
    if ((device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes ||
         device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays) &&
        buffer_cache.runtime.SetVertexBindingMap(vertex_input_mapping.bindings)) {
        // A new dense mapping changes host bindings even if the guest buffers stayed the same.
        for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
            maxwell3d->dirty.flags[VideoCommon::Dirty::VertexBuffer0 + index] = true;
        }
    }
    configure_func(this, is_indexed);
}

void GraphicsPipeline::BuildIfQueued() {
    if (build_queue) {
        build_queue->Promote(queued_build);
        // Helping uses the same compiler limit as the background workers.
        build_queue->RunNow(queued_build);
    }
}

// A pipeline can be shared by several keys, so the transition is keyed explicitly.
void GraphicsPipeline::AddTransition(GraphicsPipeline* transition,
                                     const GraphicsPipelineCacheKey& transition_key) {
    transition_keys.push_back(transition_key);
    transitions.push_back(transition);
    transition_recipe_prefixes.push_back(0);
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
    size_t attachment_read_slot{};
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
            u32 secondary_offset{};
            if constexpr (requires { desc.has_secondary; }) {
                if (desc.has_secondary) {
                    ASSERT(cbufs[desc.secondary_cbuf_index].enabled);
                    secondary = bind_cbuf(desc.secondary_cbuf_index, desc.count);
                    has_secondary = true;
                    secondary_offset = desc.secondary_cbuf_offset;
                }
            }
            return [=](u32 index) {
                const u32 delta = index << desc.size_shift;
                const u32 raw = primary.Read(desc.cbuf_offset + delta);
                return TexturePair(Tegra::Texture::ComposeTextureHandle(
                                       desc, raw, has_secondary ? secondary.Read(secondary_offset + delta) : 0),
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
                if (desc.sources.empty())
                    add_image(desc, false);
                else
                    for (const auto & source : desc.sources)
                        add_image(source, false);
            }
        }
        if constexpr (Spec::has_image_buffers) {
            for (const auto& desc : info.image_buffer_descriptors) {
                add_image(desc, false);
            }
        }
        for (const auto& desc : info.texture_descriptors) {
            if (desc.is_direct) {
                // The shader indexes the texture table itself
                if (!direct_views_initialized) {
                    for (u32 index = 0; index < desc.count; ++index) {
                        views[view_index + index] = {.index = index, .is_table = true};
                    }
                }
                view_index += desc.count;
                continue;
            }
            if (stage == NUM_STAGES - 1 && !attachment_read_kinds.empty() &&
                desc.pixel_local_read != Shader::PixelLocalRead::None) {
                attachment_read_views[attachment_read_slot++] = view_index;
            }
            const auto add_texture = [&](const auto & source) {
                const auto read_handle = prepare_handle(source);
                for (u32 index = 0; index < source.count; ++index)
                {
                    const auto handle{read_handle(index)};
                    views[view_index++] = {handle.first};

                    const VideoCommon::SamplerId sampler = memoize_samplers
                                                               ? sampler_lookup->Get(handle.second, lookup_sampler)
                                                               : lookup_sampler(handle.second);
                    samplers[sampler_index++] = sampler;
                }
            };
            if (desc.sources.empty())
                add_texture(desc);
            else
                for (const auto & source : desc.sources)
                    add_texture(source);
        }
        if constexpr (Spec::has_images)
        {
            for (const auto & desc : info.image_descriptors)
            {
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
    if (!direct_views_initialized && views.size() >= 4096) {
        filter_feedback_views = std::ranges::any_of(views, [](const auto& view) { return view.is_table; });
        if (filter_feedback_views) {
            for (size_t index = 0; index < views.size(); ++index) {
                if (!views[index].is_table) feedback_view_indices.push_back(index);
            }
            feedback_views.resize(feedback_view_indices.size());
        }
    }
    // The descriptor layout is immutable for this pipeline. FillGraphicsImageViews
    // refreshes IDs every draw without changing these direct-table indices.
    direct_views_initialized = true;
    texture_cache.FillGraphicsImageViews<Spec::has_images>(std::span(views.data(), views.size()));
    attachment_reads = attachment_read_kinds.empty()
                           ? Shader::Backend::SPIRV::NO_ATTACHMENT_READS
                           : SelectAttachmentReads(std::span(views.data(), views.size()));

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
    if (xfb) {
        ConfigureTransformFeedback(is_indexed);
    }

    guest_descriptor_queue.Acquire(num_descriptor_entries);
    texture_table_compact_pending = uses_texture_table && filter_feedback_views &&
                                    texture_table_compact_cooldown == 0;
    if (texture_table_compact_cooldown) --texture_table_compact_cooldown;
    DescriptorTableWriter table_writer{texture_table_scratch, &texture_table_writer_state,
        texture_table_compact_pending ? &texture_table_plan_scratch : nullptr};

    RescalingPushConstant rescaling;
    RenderAreaPushConstant render_area;
    const VideoCommon::SamplerId* samplers_it{samplers.data()};
    const VideoCommon::ImageViewInOut* views_it{views.data()};
    const auto prepare_stage{[&](size_t stage) LAMBDA_FORCEINLINE {
        buffer_cache.BindHostStageBuffers(stage);
        PushImageDescriptors(texture_cache, guest_descriptor_queue, stage_infos[stage], rescaling,
                             samplers_it, views_it, &texture_table_scratch, &table_writer);
        const auto& info{stage_infos[stage]};
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
        // The fragment stage is the last, its input attachments end the descriptor set
        for (u32 mask = stage_infos[4].input_attachment_mask; mask != 0; mask &= mask - 1) {
            const VideoCommon::ImageViewId id{
                texture_cache.ColorBufferViewId(static_cast<size_t>(std::countr_zero(mask)))};
            const ImageView& image_view{texture_cache.GetImageView(id)};
            guest_descriptor_queue.AddImage(id ? image_view.RenderTarget()
                                               : image_view.Handle(Shader::TextureType::Color2D));
        }
    }
    table_writer.Finish();
    texture_table_change_hint = table_writer.Changed();
    if (texture_table_compact_pending &&
        texture_table_plan_scratch.writes.size() > texture_table_plan_scratch.size / 8) {
        // Dense tables are cheaper to retain directly. Periodically retry in
        // case their occupancy changes; the worker still consumes this draw's recipe.
        texture_table_compact_cooldown = 63;
    }
    texture_cache.UpdateRenderTargets(false);
    if (filter_feedback_views) {
        // CheckFeedbackLoop already excludes every direct-table entry. Keep
        // its ordinary views in the same order without rescanning that table.
        for (size_t index = 0; index < feedback_views.size(); ++index) {
            feedback_views[index] = views[feedback_view_indices[index]];
        }
        texture_cache.CheckFeedbackLoop(feedback_views);
    } else {
        texture_cache.CheckFeedbackLoop(views);
    }
    ConfigureDraw(rescaling, render_area);
}

u32 GraphicsPipeline::SelectAttachmentReads(std::span<VideoCommon::ImageViewInOut> views) {
    using Shader::Backend::SPIRV::NO_ATTACHMENT_READS;
    texture_cache.UpdateRenderTargets(false);
    const Framebuffer* const framebuffer{texture_cache.GetFramebuffer()};
    if (framebuffer->Samples() != VK_SAMPLE_COUNT_1_BIT) {
        return NO_ATTACHMENT_READS;
    }
    // Aliased attachments are drawn through scratch images unless written, and an attachment
    // sharing its image with another one written by the draw no longer holds what was sampled
    const u32 written{framebuffer->DuplicateColorAttachmentMask() != 0 ? ColorWriteMask() : 0};
    const u32 scratch{framebuffer->ColorScratchMask(written)};
    const auto& regs{maxwell3d->regs};
    const auto& viewport{regs.viewport_transform[0]};
    // The viewport maps the screen position varying to the texel of the pixel being shaded
    const auto covers{[&](const VideoCommon::Extent3D& size) {
        if (regs.viewport_scale_offset_enabled == 0 ||
            viewport.swizzle.x != Maxwell::ViewportSwizzle::PositiveX ||
            (viewport.swizzle.y != Maxwell::ViewportSwizzle::PositiveY &&
             (device.IsNvViewportSwizzleSupported() ||
              viewport.swizzle.y != Maxwell::ViewportSwizzle::NegativeY))) {
            return false;
        }
        const VkViewport host{GetViewportState(device, regs, 0, 1.0f, 0.0f)};
        const f32 width{static_cast<f32>(size.width)};
        const f32 height{static_cast<f32>(size.height)};
        return host.x == 0.0f && host.width == width && host.y == height && host.height == -height;
    }};
    const u32 mask{stage_infos[NUM_STAGES - 1].input_attachment_mask};
    u32 reads{NO_ATTACHMENT_READS};
    for (size_t slot = 0; slot < attachment_read_kinds.size(); ++slot) {
        VideoCommon::ImageViewInOut& view{views[attachment_read_views[slot]]};
        if (!view.id) {
            continue;
        }
        const ImageView& sampled{texture_cache.GetImageView(view.id)};
        for (u32 index = 0; index < Maxwell::NumRenderTargets; ++index) {
            const VideoCommon::ImageViewId target_id{texture_cache.ColorBufferViewId(index)};
            if (((mask >> index) & 1) == 0 || !target_id) {
                continue;
            }
            const ImageView& target{texture_cache.GetImageView(target_id)};
            if (sampled.image_id != target.image_id || sampled.range.base != target.range.base ||
                ((scratch >> index) & 1) != 0 ||
                (framebuffer->ColorAttachmentAliasMask(index) & written) != 0) {
                continue;
            }
            if (sampled.type == VideoCommon::ImageViewType::e2D &&
                sampled.range.extent.levels == 1 && sampled.range.extent.layers == 1 &&
                sampled.format == target.format && sampled.HasIdentitySwizzle() &&
                (attachment_read_kinds[slot] != Shader::PixelLocalRead::ScreenPosition ||
                 covers(sampled.size))) {
                const u32 shift{static_cast<u32>(slot) * 4};
                reads = (reads & ~(0xFU << shift)) | (index << shift);
                // Sampling nothing, the read sees what earlier draws wrote to the attachment
                view.id = VideoCommon::NULL_IMAGE_VIEW_ID;
            }
            break;
        }
    }
    return reads;
}

void GraphicsPipeline::ConfigureVtg(bool is_indexed) {
    using Shader::VTG_LOCAL_SIZE;
    namespace DrawInfo = Shader::VtgDrawInfo;
    static constexpr size_t FRAGMENT_STAGE = 4;
    static constexpr u32 MAX_INVOCATIONS = 1U << 20;
    static constexpr VkDeviceSize MAX_SCRATCH_SIZE = 256ULL << 20;

    VtgState& state{*vtg};
    state.index_count = 0;

    auto& views = descriptor_views;
    auto& samplers = descriptor_samplers;
    size_t sampler_index{};
    size_t view_index{};

    texture_cache.SynchronizeGraphicsDescriptors();

    buffer_cache.SetUniformBuffersState(state.uniform_masks, &state.uniform_sizes);

    // The guest resources of the stages running as compute are bound as the stages they replace
    const std::array<std::pair<size_t, const Shader::Info*>, 3> stages{{
        {0, &state.stages[VtgPipelineInfo::VERTEX].guest_info},
        {3, &state.stages[VtgPipelineInfo::GEOMETRY].guest_info},
        {FRAGMENT_STAGE, &stage_infos[FRAGMENT_STAGE]},
    }};
    const auto& regs{maxwell3d->regs};
    const bool via_header_index{regs.sampler_binding == Maxwell::SamplerBinding::ViaHeaderBinding};
    for (const auto& [stage, info_pointer] : stages) {
        const Shader::Info& info{*info_pointer};
        buffer_cache.UnbindGraphicsStorageBuffers(stage);
        size_t ssbo_index{};
        for (const auto& desc : info.storage_buffers_descriptors) {
            ASSERT(desc.count == 1);
            buffer_cache.BindGraphicsStorageBuffer(stage, ssbo_index, desc.cbuf_index,
                                                   desc.cbuf_offset, desc.is_written);
            ++ssbo_index;
        }
        const auto& cbufs{maxwell3d->state.shader_stages[stage].const_buffers};
        const auto read_handle{[&](const auto& desc, u32 index) {
            ASSERT(cbufs[desc.cbuf_index].enabled);
            const u32 index_offset{index << desc.size_shift};
            const u32 offset{desc.cbuf_offset + index_offset};
            const GPUVAddr addr{cbufs[desc.cbuf_index].address + offset};
            if constexpr (requires { desc.has_secondary; }) {
                if (desc.has_secondary) {
                    ASSERT(cbufs[desc.secondary_cbuf_index].enabled);
                    const u32 second_offset{desc.secondary_cbuf_offset + index_offset};
                    const GPUVAddr separate_addr{cbufs[desc.secondary_cbuf_index].address +
                                                 second_offset};
                    const u32 lhs_raw{gpu_memory->Read<u32>(addr) << desc.shift_left};
                    const u32 rhs_raw{gpu_memory->Read<u32>(separate_addr)
                                      << desc.secondary_shift_left};
                    return TexturePair(lhs_raw | rhs_raw | desc.handle_or_bits, via_header_index);
                }
                return TexturePair(Tegra::Texture::ComposeTextureHandle(
                                       desc, gpu_memory->Read<u32>(addr)), via_header_index);
            }
            return TexturePair(gpu_memory->Read<u32>(addr), via_header_index);
        }};
        const auto add_image{[&](const auto& desc, bool blacklist) {
            for (u32 index = 0; index < desc.count; ++index) {
                const auto handle{read_handle(desc, index)};
                views[view_index++] = {
                    .index = handle.first,
                    .blacklist = blacklist,
                    .id = {},
                };
            }
        }};
        for (const auto& desc : info.texture_buffer_descriptors) {
            if (desc.sources.empty())
                add_image(desc, false);
            else
                for (const auto & source : desc.sources)
                    add_image(source, false);
        }
        for (const auto& desc : info.image_buffer_descriptors) {
            add_image(desc, false);
        }
        for (const auto& desc : info.texture_descriptors) {
            if (desc.is_direct) {
                for (u32 index = 0; index < desc.count; ++index) {
                    views[view_index++] = {.index = index, .is_table = true};
                }
                continue;
            }
            for (u32 index = 0; index < desc.count; ++index) {
                const auto handle{desc.sources.empty() ? read_handle(desc, index) : read_handle(desc.sources[index], 0)};
                views[view_index++] = {handle.first};
                samplers[sampler_index++] = texture_cache.GetGraphicsSamplerId(handle.second);
            }
        }
        for (const auto& desc : info.image_descriptors) {
            add_image(desc, desc.is_written);
        }
    }
    ASSERT(view_index == views.size());
    ASSERT(sampler_index == samplers.size());
    texture_cache.FillGraphicsImageViews<true>(std::span(views.data(), views.size()));

    VideoCommon::ImageViewInOut* texture_buffer_it{views.data()};
    for (const auto& [stage, info_pointer] : stages) {
        const Shader::Info& info{*info_pointer};
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
        for (const auto& desc : info.texture_buffer_descriptors) {
            add_buffer(desc);
        }
        for (const auto& desc : info.image_buffer_descriptors) {
            add_buffer(desc);
        }
        texture_buffer_it += Shader::NumDescriptors(info.texture_descriptors);
        texture_buffer_it += Shader::NumDescriptors(info.image_descriptors);
    }

    buffer_cache.UpdateGraphicsBuffers(is_indexed);
    buffer_cache.BindHostGeometryBuffers(is_indexed);

    const auto& draw_state{maxwell3d->draw_manager->GetDrawState()};
    const u32 vertex_count{is_indexed ? draw_state.index_buffer.count
                                      : draw_state.vertex_buffer.count};
    const u32 instance_count{state.instance_count};
    const u32 primitive_count{Shader::VtgPrimitiveCount(state.topology, vertex_count)};
    const u64 total_vertices{u64{vertex_count} * instance_count};
    const u64 total_primitives{u64{primitive_count} * instance_count};
    const u32 vertices_per_primitive{state.geometry_bindings.vertices_per_primitive};
    const u32 indices_per_primitive{state.geometry_bindings.indices_per_primitive};
    const bool fits{total_vertices <= MAX_INVOCATIONS && total_primitives <= MAX_INVOCATIONS &&
                    (total_vertices * state.vertex_stride +
                     total_primitives * (u64{vertices_per_primitive} * state.geometry_stride +
                                         indices_per_primitive)) *
                            sizeof(u32) <=
                        MAX_SCRATCH_SIZE};
    const bool runs{total_primitives != 0 && fits};
    if (!fits) {
        LOG_WARNING(Render_Vulkan, "Draw of {} vertices and {} instances is too large to run as "
                                   "compute", vertex_count, instance_count);
    }
    // Invocations past the end of the draw write into the padding of the buffers
    const u32 vertex_invocations{
        runs ? Common::AlignUp(static_cast<u32>(total_vertices), VTG_LOCAL_SIZE) : VTG_LOCAL_SIZE};
    const u32 geometry_invocations{
        runs ? Common::AlignUp(static_cast<u32>(total_primitives), VTG_LOCAL_SIZE)
             : VTG_LOCAL_SIZE};
    const u32 output_vertices{geometry_invocations * vertices_per_primitive};
    const u32 output_indices{geometry_invocations * indices_per_primitive};
    const bool emits_points{state.geometry_bindings.output_topology ==
                            Shader::OutputTopology::PointList};

    const VkDeviceSize vertex_size{u64{vertex_invocations} * state.vertex_stride * sizeof(u32)};
    // One more vertex is reserved, used by the indices of points that are not emitted
    const VkDeviceSize geometry_size{u64{output_vertices + 1} * state.geometry_stride *
                                     sizeof(u32)};
    const VkDeviceSize index_size{u64{output_indices} * sizeof(u32)};
    const auto [vertex_buffer, vertex_offset]{state.scratch->Allocate(vertex_size)};
    const auto [geometry_buffer, geometry_offset]{state.scratch->Allocate(geometry_size)};
    const auto [index_buffer, index_offset]{state.scratch->Allocate(index_size)};

    // Describe the draw to the stages running as compute
    const StagingBufferRef info_ref{state.staging_pool->Request(
        DrawInfo::NUM_WORDS * sizeof(u32), MemoryUsage::Upload)};
    const VtgState::Buffer info_buffer{info_ref.buffer, info_ref.offset,
                                       DrawInfo::NUM_WORDS * sizeof(u32)};
    std::array<u32, DrawInfo::NUM_WORDS> words{};
    const VkDeviceSize alignment{
        std::max<VkDeviceSize>(device.GetStorageBufferAlignment(), sizeof(u32))};
    // Guest buffers are bound from an aligned offset, the remainder is added by the shader
    const auto guest_buffer{[&](const std::tuple<Buffer*, u32, u32>& binding, u32& misalignment) {
        const auto [buffer, offset, size]{binding};
        misalignment = 0;
        if (!buffer || buffer->Handle() == VK_NULL_HANDLE) {
            return info_buffer;
        }
        const VkDeviceSize aligned{Common::AlignDown(VkDeviceSize{offset}, alignment)};
        misalignment = static_cast<u32>(offset - aligned);
        const VkDeviceSize wanted{Common::AlignUp(VkDeviceSize{size} + misalignment, 8)};
        const VkDeviceSize available{
            Common::AlignDown(VkDeviceSize{buffer->SizeBytes()} - aligned, sizeof(u32))};
        return VtgState::Buffer{buffer->Handle(), aligned, std::min(wanted, available)};
    }};
    static_vector<VtgState::Buffer, 3 + DrawInfo::NUM_ATTRIBUTES> vertex_buffers;
    std::array<u32, Maxwell::NumVertexArrays> misalignments{};
    u32 index_misalignment{};
    const u32 index_bytes{is_indexed ? draw_state.index_buffer.FormatSizeInBytes() : 0};
    vertex_buffers.push_back(info_buffer);
    vertex_buffers.push_back({vertex_buffer, vertex_offset, vertex_size});
    vertex_buffers.push_back(is_indexed
                                 ? guest_buffer(buffer_cache.IndexBufferBinding(),
                                                index_misalignment)
                                 : info_buffer);
    for (const auto& [stream, binding] : state.vertex_bindings.vertex_buffers) {
        vertex_buffers.push_back(
            guest_buffer(buffer_cache.VertexBufferBinding(stream), misalignments[stream]));
    }
    words[DrawInfo::VERTEX_COUNT] = vertex_count;
    words[DrawInfo::INSTANCE_COUNT] = std::max(instance_count, 1U);
    words[DrawInfo::BASE_VERTEX] =
        is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first;
    words[DrawInfo::BASE_INSTANCE] = draw_state.base_instance;
    words[DrawInfo::INDEX_SIZE] = index_bytes;
    words[DrawInfo::INDEX_OFFSET] =
        index_misalignment + (is_indexed ? draw_state.index_buffer.first * index_bytes : 0);
    words[DrawInfo::PRIMITIVE_COUNT] = primitive_count;
    words[DrawInfo::INDEX_MASK] =
        index_bytes == 1 ? 0xffU : (index_bytes == 2 ? 0xffffU : 0xffffffffU);
    for (size_t index = 0; index < DrawInfo::NUM_ATTRIBUTES; ++index) {
        const auto& attribute{regs.vertex_attrib_format[index]};
        const u32 stream{attribute.buffer};
        u32* const attribute_words{&words[DrawInfo::ATTRIBUTES + index * DrawInfo::ATTRIBUTE_WORDS]};
        attribute_words[DrawInfo::ATTRIBUTE_OFFSET] = misalignments[stream] + attribute.offset;
        attribute_words[DrawInfo::ATTRIBUTE_STRIDE] = regs.vertex_streams[stream].stride;
        attribute_words[DrawInfo::ATTRIBUTE_INSTANCED] =
            regs.vertex_stream_instances.IsInstancingEnabled(stream) ? 1 : 0;
        attribute_words[DrawInfo::ATTRIBUTE_DIVISOR] = regs.vertex_streams[stream].frequency;
    }
    std::memcpy(info_ref.mapped_span.data(), words.data(), sizeof(words));

    const std::array<VtgState::Buffer, 4> geometry_buffers{{
        info_buffer,
        {vertex_buffer, vertex_offset, vertex_size},
        {geometry_buffer, geometry_offset, geometry_size},
        {index_buffer, index_offset, index_size},
    }};

    if (runs && !build_complete.load(std::memory_order_acquire)) {
        scheduler.Record([this](vk::CommandBuffer) {
            BuildIfQueued();
            std::unique_lock lock{build_mutex};
            build_condvar.wait(lock,
                               [this] { return build_complete.load(std::memory_order_acquire); });
        });
    }
    // Commands on the upload buffer run before the ones of the execution context. The stages are
    // recorded there, leaving the render pass open, unless they read what these commands write.
    bool reorder{!videoSettings.disable_buffer_reorder};
    const VideoCommon::SamplerId* samplers_it{samplers.data()};
    const VideoCommon::ImageViewInOut* views_it{views.data()};
    const auto run_stage{[&](size_t index, size_t guest_stage,
                             std::span<const VtgState::Buffer> buffers, u32 invocations) {
        VtgState::Stage& stage{state.stages[index]};
        guest_descriptor_queue.Acquire(stage.num_descriptor_entries);
        buffer_cache.BindHostStageUniformAndStorageBuffers(guest_stage);
        for (const VtgState::Buffer& buffer : buffers) {
            guest_descriptor_queue.AddBuffer(buffer.buffer, buffer.offset, buffer.size);
        }
        const bool is_rescaling{!stage.guest_info.texture_descriptors.empty() ||
                                !stage.guest_info.image_descriptors.empty()};
        const bool reads_images{is_rescaling ||
                                !stage.guest_info.texture_buffer_descriptors.empty() ||
                                !stage.guest_info.image_buffer_descriptors.empty()};
        // Only buffers have been added to the queue when the stage has no images
        reorder = reorder && !reads_images &&
                  std::ranges::none_of(guest_descriptor_queue.Entries(),
                                       [this](const DescriptorUpdateEntry& entry) {
                                           return scheduler.IsBufferWritten(entry.buffer.buffer);
                                       });
        buffer_cache.BindHostStageTextureBuffers(guest_stage);
        RescalingPushConstant stage_rescaling;
        PushImageDescriptors(texture_cache, guest_descriptor_queue, stage.guest_info,
                             stage_rescaling, samplers_it, views_it);
        if (!runs) {
            return;
        }
        const void* const descriptor_data{guest_descriptor_queue.UpdateData()};
        if (!reorder) {
            scheduler.RequestOutsideRenderPassOperationContext();
        }
        scheduler.RecordWithUploadBuffer([this, &stage, descriptor_data, is_rescaling, reorder,
                                          rescaling_data = stage_rescaling.Data(),
                                          groups = invocations / VTG_LOCAL_SIZE](
                                             vk::CommandBuffer main_cmdbuf,
                                             vk::CommandBuffer upload_cmdbuf) {
            if (!IsBuilt()) {
                return;
            }
            const vk::CommandBuffer cmdbuf{reorder ? upload_cmdbuf : main_cmdbuf};
            if (reorder) {
                // Moving the stage ahead also moves it ahead of guest barriers recorded in this
                // context. Shader writes from previous submissions have no barrier of their own,
                // order them explicitly. Writes of this context are excluded by IsBufferWritten.
                static constexpr VkMemoryBarrier barrier{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
            }
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *stage.pipeline);
            if (is_rescaling) {
                cmdbuf.PushConstants(*stage.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                     RESCALING_LAYOUT_WORDS_OFFSET, sizeof(rescaling_data),
                                     rescaling_data.data());
            }
            const VkDescriptorSet descriptor_set{stage.descriptor_allocator.Commit()};
            device.GetLogical().UpdateDescriptorSet(descriptor_set,
                                                    *stage.descriptor_update_template,
                                                    descriptor_data);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *stage.pipeline_layout, 0,
                                      descriptor_set, nullptr);
            cmdbuf.Dispatch(groups, 1, 1);
            Scheduler::ComputeMemoryBarrier(cmdbuf);
        });
    }};
    run_stage(VtgPipelineInfo::VERTEX, 0, vertex_buffers, vertex_invocations);
    if (runs) {
        // Index slots that are not written end the strip, or draw a discarded vertex as a point
        const u32 unused_index{emits_points ? output_vertices : Shader::VTG_RESTART_INDEX};
        const VkDeviceSize sentinel_offset{geometry_offset +
                                           u64{output_vertices} * state.geometry_stride *
                                               sizeof(u32)};
        const VkDeviceSize sentinel_size{u64{state.geometry_stride} * sizeof(u32)};
        // This has to precede the geometry stage, which may still leave the upload buffer
        if (!reorder) {
            scheduler.RequestOutsideRenderPassOperationContext();
        }
        scheduler.RecordWithUploadBuffer([index_buffer = index_buffer, index_offset = index_offset,
                                          index_size, unused_index,
                                          geometry_buffer = geometry_buffer, sentinel_offset,
                                          sentinel_size, emits_points,
                                          reorder](vk::CommandBuffer main_cmdbuf,
                                                   vk::CommandBuffer upload_cmdbuf) {
            static constexpr u32 QUIET_NAN = 0x7fc00000;
            const vk::CommandBuffer cmdbuf{reorder ? upload_cmdbuf : main_cmdbuf};
            cmdbuf.FillBuffer(index_buffer, index_offset, index_size, unused_index);
            if (emits_points) {
                cmdbuf.FillBuffer(geometry_buffer, sentinel_offset, sentinel_size, QUIET_NAN);
            }
            static constexpr VkMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
        });
    }
    run_stage(VtgPipelineInfo::GEOMETRY, 3, geometry_buffers, geometry_invocations);

    // The generated vertex shader reads the vertices written by the geometry stage
    guest_descriptor_queue.Acquire(num_descriptor_entries);
    guest_descriptor_queue.AddBuffer(geometry_buffer, geometry_offset, geometry_size);
    texture_table_scratch.clear();

    RescalingPushConstant rescaling;
    RenderAreaPushConstant render_area;
    buffer_cache.BindHostStageBuffers(FRAGMENT_STAGE);
    PushImageDescriptors(texture_cache, guest_descriptor_queue, stage_infos[FRAGMENT_STAGE],
                         rescaling, samplers_it, views_it, &texture_table_scratch);
    if (stage_infos[FRAGMENT_STAGE].uses_render_area) {
        render_area.uses_render_area = true;
        render_area.words = {static_cast<float>(regs.surface_clip.width),
                             static_cast<float>(regs.surface_clip.height)};
    }
    texture_cache.UpdateRenderTargets(false);
    texture_cache.CheckFeedbackLoop(views);
    ConfigureDraw(rescaling, render_area);

    if (runs) {
        state.index_buffer = index_buffer;
        state.index_offset = index_offset;
        state.index_count = static_cast<u32>(total_primitives) * indices_per_primitive;
    }
}

void GraphicsPipeline::ConfigureTransformFeedback(bool is_indexed) {
    using Shader::VTG_LOCAL_SIZE;
    namespace DrawInfo = Shader::VtgDrawInfo;
    static constexpr u64 MAX_INVOCATIONS = 1U << 22;

    XfbState& state{*xfb};
    const auto& regs{maxwell3d->regs};
    if (regs.transform_feedback_enabled == 0 || state.instance_count == 0) {
        return;
    }
    const auto& draw_state{maxwell3d->draw_manager->GetDrawState()};
    const u32 vertex_count{is_indexed ? draw_state.index_buffer.count
                                      : draw_state.vertex_buffer.count};
    const u32 per_instance{Shader::VtgPrimitiveCount(state.layout.topology, vertex_count) *
                           Shader::VtgCapturedVertices(state.layout.topology)};
    // Like the hardware, the capture stops at the first vertex that does not fit in a buffer
    u64 total{u64{per_instance} * state.instance_count};
    auto& targets{buffer_cache.runtime.EmulatedTransformFeedbackBuffers()};
    for (size_t index = 0; index < targets.size(); ++index) {
        const u32 stride{state.layout.strides[index]};
        if (stride == 0) {
            continue;
        }
        const auto& target{targets[index]};
        const u64 size{std::min<u64>(target.size, target.capacity)};
        const u64 room{target.buffer != VK_NULL_HANDLE && size > target.written
                           ? size - target.written
                           : 0};
        total = std::min(total, room / stride);
    }
    if (total > MAX_INVOCATIONS) {
        LOG_WARNING(Render_Vulkan, "Transform feedback of {} vertices is too large to emulate",
                    total);
        return;
    }
    if (total == 0) {
        return;
    }

    // Describe the draw to the stage running as compute
    const StagingBufferRef info_ref{
        state.staging_pool->Request(DrawInfo::NUM_WORDS * sizeof(u32), MemoryUsage::Upload)};
    const VtgState::Buffer info_buffer{info_ref.buffer, info_ref.offset,
                                       DrawInfo::NUM_WORDS * sizeof(u32)};
    std::array<u32, DrawInfo::NUM_WORDS> words{};
    const VkDeviceSize alignment{
        std::max<VkDeviceSize>(device.GetStorageBufferAlignment(), sizeof(u32))};
    // Guest buffers are bound from an aligned offset, the remainder is added by the shader
    const auto aligned_buffer{[&](VkBuffer handle, u64 offset, u64 size, u64 capacity,
                                  u32& misalignment) {
        const VkDeviceSize aligned{Common::AlignDown(VkDeviceSize{offset}, alignment)};
        misalignment = static_cast<u32>(offset - aligned);
        const VkDeviceSize available{
            Common::AlignDown(VkDeviceSize{capacity} + misalignment, sizeof(u32))};
        const VkDeviceSize wanted{Common::AlignUp(VkDeviceSize{size} + misalignment, 8)};
        return VtgState::Buffer{handle, aligned, std::min(wanted, available)};
    }};
    const auto guest_buffer{[&](const std::tuple<Buffer*, u32, u32>& binding, u32& misalignment) {
        const auto [buffer, offset, size]{binding};
        misalignment = 0;
        if (!buffer || buffer->Handle() == VK_NULL_HANDLE) {
            return info_buffer;
        }
        return aligned_buffer(buffer->Handle(), offset, size, buffer->SizeBytes() - offset,
                              misalignment);
    }};
    static_vector<VtgState::Buffer, 3 + DrawInfo::NUM_ATTRIBUTES + DrawInfo::NUM_XFB_BUFFERS>
        buffers;
    std::array<u32, Maxwell::NumVertexArrays> misalignments{};
    u32 index_misalignment{};
    const u32 index_bytes{is_indexed ? draw_state.index_buffer.FormatSizeInBytes() : 0};
    buffers.push_back(info_buffer);
    // The stage writes no varyings of its own
    buffers.push_back(info_buffer);
    buffers.push_back(is_indexed
                          ? guest_buffer(buffer_cache.IndexBufferBinding(), index_misalignment)
                          : info_buffer);
    for (const auto& [stream, binding] : state.bindings.vertex_buffers) {
        buffers.push_back(
            guest_buffer(buffer_cache.VertexBufferBinding(stream), misalignments[stream]));
    }
    for (size_t index = 0; index < targets.size(); ++index) {
        auto& target{targets[index]};
        const u32 stride{state.layout.strides[index]};
        if (stride == 0) {
            buffers.push_back(info_buffer);
            continue;
        }
        u32 misalignment{};
        buffers.push_back(aligned_buffer(target.buffer, target.offset, target.size,
                                         target.capacity, misalignment));
        words[DrawInfo::XFB_OFFSETS + index] = misalignment + target.written;
        target.written += static_cast<u32>(total * stride);
    }
    words[DrawInfo::VERTEX_COUNT] = vertex_count;
    words[DrawInfo::INSTANCE_COUNT] = state.instance_count;
    words[DrawInfo::BASE_VERTEX] =
        is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first;
    words[DrawInfo::BASE_INSTANCE] = draw_state.base_instance;
    words[DrawInfo::INDEX_SIZE] = index_bytes;
    words[DrawInfo::INDEX_OFFSET] =
        index_misalignment + (is_indexed ? draw_state.index_buffer.first * index_bytes : 0);
    words[DrawInfo::INDEX_MASK] =
        index_bytes == 1 ? 0xffU : (index_bytes == 2 ? 0xffffU : 0xffffffffU);
    words[DrawInfo::XFB_VERTICES] = per_instance;
    words[DrawInfo::XFB_TOTAL] = static_cast<u32>(total);
    for (size_t index = 0; index < DrawInfo::NUM_ATTRIBUTES; ++index) {
        const auto& attribute{regs.vertex_attrib_format[index]};
        const u32 stream{attribute.buffer};
        u32* const attribute_words{&words[DrawInfo::ATTRIBUTES + index * DrawInfo::ATTRIBUTE_WORDS]};
        attribute_words[DrawInfo::ATTRIBUTE_OFFSET] = misalignments[stream] + attribute.offset;
        attribute_words[DrawInfo::ATTRIBUTE_STRIDE] = regs.vertex_streams[stream].stride;
        attribute_words[DrawInfo::ATTRIBUTE_INSTANCED] =
            regs.vertex_stream_instances.IsInstancingEnabled(stream) ? 1 : 0;
        attribute_words[DrawInfo::ATTRIBUTE_DIVISOR] = regs.vertex_streams[stream].frequency;
    }
    std::memcpy(info_ref.mapped_span.data(), words.data(), sizeof(words));

    if (!build_complete.load(std::memory_order_acquire)) {
        scheduler.Record([this](vk::CommandBuffer) {
            BuildIfQueued();
            std::unique_lock lock{build_mutex};
            build_condvar.wait(lock,
                               [this] { return build_complete.load(std::memory_order_acquire); });
        });
    }
    // The stage reads the resources of the vertex stage, which come first
    const VideoCommon::SamplerId* samplers_it{descriptor_samplers.data()};
    const VideoCommon::ImageViewInOut* views_it{descriptor_views.data()};
    VtgState::Stage& stage{state.stage};
    guest_descriptor_queue.Acquire(stage.num_descriptor_entries);
    buffer_cache.BindHostStageUniformAndStorageBuffers(0);
    for (const VtgState::Buffer& buffer : buffers) {
        guest_descriptor_queue.AddBuffer(buffer.buffer, buffer.offset, buffer.size);
    }
    buffer_cache.BindHostStageTextureBuffers(0);
    RescalingPushConstant stage_rescaling;
    PushImageDescriptors(texture_cache, guest_descriptor_queue, stage.guest_info, stage_rescaling,
                         samplers_it, views_it);
    const void* const descriptor_data{guest_descriptor_queue.UpdateData()};
    const bool is_rescaling{!stage.guest_info.texture_descriptors.empty() ||
                            !stage.guest_info.image_descriptors.empty()};
    const u32 groups{static_cast<u32>(Common::DivCeil(total, u64{VTG_LOCAL_SIZE}))};
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, &stage, descriptor_data, is_rescaling,
                      rescaling_data = stage_rescaling.Data(), groups](vk::CommandBuffer cmdbuf) {
        if (!IsBuilt()) {
            return;
        }
        // Earlier draws may still read or write the captured buffers
        static constexpr VkMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, before);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *stage.pipeline);
        if (is_rescaling) {
            cmdbuf.PushConstants(*stage.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                                 RESCALING_LAYOUT_WORDS_OFFSET, sizeof(rescaling_data),
                                 rescaling_data.data());
        }
        const VkDescriptorSet descriptor_set{stage.descriptor_allocator.Commit()};
        device.GetLogical().UpdateDescriptorSet(descriptor_set, *stage.descriptor_update_template,
                                                descriptor_data);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *stage.pipeline_layout, 0,
                                  descriptor_set, nullptr);
        cmdbuf.Dispatch(groups, 1, 1);
        static constexpr VkMemoryBarrier after{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, after);
    });
}

VkDescriptorSet GraphicsPipeline::WorkerDescriptorSet(const void* data, u64 tick) {
    const VkDescriptorSet set{descriptor_allocator.Commit()};
    const auto& dev = device.GetLogical();
    if (large_image_entries.empty()) {
        dev.UpdateDescriptorSet(set, *descriptor_update_template, data);
        return set;
    }
    auto& cache = guest_descriptor_queue.CopyCache();
    cache.Begin(tick);
    boost::container::small_vector<VkWriteDescriptorSet, 8> writes;
    boost::container::small_vector<VkCopyDescriptorSet, 8> copies;
    boost::container::small_vector<const VkDescriptorUpdateTemplateEntry*, 8> uncached;
    const auto bytes_for = [data](const VkDescriptorUpdateTemplateEntry& entry) {
        return std::span{static_cast<const std::byte*>(data) + entry.offset,
                         entry.descriptorCount * sizeof(DescriptorUpdateEntry)};
    };
    static_assert(sizeof(DescriptorUpdateEntry) == sizeof(VkDescriptorImageInfo));
    for (const auto& entry : large_image_entries) {
        const auto source = cache.Find(entry.descriptorType, bytes_for(entry));
        if (source) {
            // Both sets belong to this tick and are immutable once populated.
            // Commit() still protects the destination against in-flight reuse.
            copies.push_back({
                .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
                .pNext = nullptr,
                .srcSet = source->set,
                .srcBinding = source->binding,
                .srcArrayElement = 0,
                .dstSet = set,
                .dstBinding = entry.dstBinding,
                .dstArrayElement = 0,
                .descriptorCount = entry.descriptorCount,
            });
        } else {
            uncached.push_back(&entry);
            writes.push_back({
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = set,
                .dstBinding = entry.dstBinding,
                .dstArrayElement = 0,
                .descriptorCount = entry.descriptorCount,
                .descriptorType = entry.descriptorType,
                .pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(
                    static_cast<const std::byte*>(data) + entry.offset),
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            });
        }
    }
    if (copies.empty()) {
        dev.UpdateDescriptorSet(set, *descriptor_update_template, data);
    } else {
        if (small_descriptor_update_template) {
            dev.UpdateDescriptorSet(set, *small_descriptor_update_template, data);
        }
        dev.UpdateDescriptorSets(writes, copies);
    }
    // Publish only after every write/copy has completed on the host. These
    // snapshots own their bytes; the descriptor upload block can be retired.
    for (const auto* entry : uncached) {
        if (!cache.Find(entry->descriptorType, bytes_for(*entry))) {
            cache.Store(entry->descriptorType, bytes_for(*entry), {set, entry->dstBinding});
        }
    }
    return set;
}

u32 GraphicsPipeline::ColorWriteMask() const {
    u32 color_write_mask{};
    const auto& regs = maxwell3d->regs;
    const Shader::Info& fragment_info = stage_infos[4];
    for (size_t index = 0; index < Maxwell::NumRenderTargets; ++index) {
        const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
        if ((mask.R || mask.G || mask.B || mask.A) && fragment_info.stores_frag_color[index]) {
            color_write_mask |= 1U << index;
        }
    }
    return color_write_mask;
}

void GraphicsPipeline::ConfigureDraw(const RescalingPushConstant& rescaling,
                                     const RenderAreaPushConstant& render_area) {
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    u32 color_scratch_mask{};
    if (device.GetDriverID() == VK_DRIVER_ID_MOLTENVK) {
        const u32 duplicate_mask = framebuffer->DuplicateColorAttachmentMask();
        if (duplicate_mask != 0) {
            // Avoid independent Metal tile copies of
            // the same image. Keep the render-pass shape, using scratch for masked slots.
            color_scratch_mask = framebuffer->ColorScratchMask(ColorWriteMask());
        }
    }
    scheduler.RequestRenderpass(framebuffer, color_scratch_mask);
    if (attachment_reads != Shader::Backend::SPIRV::NO_ATTACHMENT_READS) {
        // Reads of the pixel being shaded only wait for earlier draws to the same region
        scheduler.Record([](vk::CommandBuffer cmdbuf) {
            const VkMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                   VK_DEPENDENCY_BY_REGION_BIT, barrier);
        });
    }

    if (!build_complete.load(std::memory_order_acquire)) {
        // Completion includes failure, so a failed build cannot strand this worker.
        scheduler.Record([this](vk::CommandBuffer) {
            BuildIfQueued();
            std::unique_lock lock{build_mutex};
            build_condvar.wait(lock, [this] { return build_complete.load(std::memory_order_acquire); });
        });
    }
    const bool is_rescaling{texture_cache.IsRescaling()};
    const bool update_rescaling{scheduler.UpdateRescaling(is_rescaling)};
    const bool bind_pipeline{scheduler.UpdateGraphicsPipeline(this)};
    const void* const descriptor_data{guest_descriptor_queue.UpdateData()};
    const u64 descriptor_tick{scheduler.CurrentTick()};
    const bool compact_table = std::exchange(texture_table_compact_pending, false);
    std::shared_ptr<const DescriptorTablePlan> table_plan;
    if (uses_texture_table) {
        // A committed set can be recycled once its tick completes, so it is not kept across
        // ticks. Within a tick, draws share it for as long as the tables hold the same handles.
        const u64 tick{scheduler.CurrentTick()};
        const auto same_entry{[](const VkDescriptorImageInfo& lhs, const VkDescriptorImageInfo& rhs) {
            return lhs.sampler == rhs.sampler && lhs.imageView == rhs.imageView;
        }};
        bool table_changed;
        if (compact_table) {
            table_changed = !texture_table_was_compact || !texture_table_plan_data ||
                            !(texture_table_plan_scratch == *texture_table_plan_data);
            if (table_changed) texture_table_plan_data =
                std::make_shared<const DescriptorTablePlan>(texture_table_plan_scratch);
            table_plan = texture_table_plan_data;
        } else {
            table_changed = texture_table_was_compact || !texture_table_data ||
                (texture_table_change_hint ? *texture_table_change_hint :
                 !std::ranges::equal(texture_table_scratch, *texture_table_data, same_entry));
            if (table_changed) texture_table_data =
                std::make_shared<const std::vector<VkDescriptorImageInfo>>(texture_table_scratch);
        }
        texture_table_was_compact = compact_table;
        texture_table_change_hint.reset();
        // A new tick still allocates a fresh descriptor set on the worker, but
        // identical immutable host data can be shared with the preceding tick.
        if (table_changed || texture_table_tick != tick) {
            texture_table_tick = tick;
            ++texture_table_version;
        }
    }
    if (!uses_texture_table) {
        scheduler.Record([this, descriptor_data, descriptor_tick, bind_pipeline,
                          rescaling_data = rescaling.Data(),
                          is_rescaling, update_rescaling, reads = attachment_reads,
                          uses_render_area = render_area.uses_render_area,
                          render_area_data = render_area.words](vk::CommandBuffer cmdbuf) {
            if (!IsBuilt()) {
                return;
            }
            if (bind_pipeline) {
                cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline);
            }
            const bool update_factor{update_rescaling || bind_pipeline};
            const auto data = Shader::Backend::SPIRV::MakeGraphicsPushConstants(
                rescaling_data, is_rescaling ? videoSettings.resolution_info.down_factor : 1.0f,
                reads, render_area_data);
            const u32 size = uses_render_area ? sizeof(data) :
                !attachment_read_kinds.empty() ? RESCALING_LAYOUT_ATTACHMENT_READS_OFFSET + sizeof(reads) :
                sizeof(rescaling_data) + (update_factor ? sizeof(f32) : 0);
            cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_ALL_GRAPHICS,
                                 RESCALING_LAYOUT_WORDS_OFFSET, size, &data);
            // An empty set 0 has a layout but nothing to update
            if (!descriptor_update_template) {
                return;
            }
            if (uses_push_descriptor) {
                cmdbuf.PushDescriptorSetWithTemplateKHR(*descriptor_update_template, *pipeline_layout,
                                                        0, descriptor_data);
            } else {
                const VkDescriptorSet descriptor_set{
                    WorkerDescriptorSet(descriptor_data, descriptor_tick)};
                cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline_layout, 0,
                                          descriptor_set, nullptr);
            }
        });
    } else {
        scheduler.Record([this, descriptor_data, descriptor_tick, bind_pipeline,
                          rescaling_data = rescaling.Data(),
                          is_rescaling, update_rescaling,
                          table_data = compact_table ? nullptr : texture_table_data, table_plan,
                          table_version = texture_table_version, reads = attachment_reads,
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
            if (!attachment_read_kinds.empty()) {
                cmdbuf.PushConstants(*pipeline_layout, VK_SHADER_STAGE_ALL_GRAPHICS,
                                     RESCALING_LAYOUT_ATTACHMENT_READS_OFFSET, sizeof(reads),
                                     &reads);
            }
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
            if (table_data || table_plan) {
                if (texture_table_set_version != table_version) {
                    texture_table_set = texture_table_allocator.Commit();
                    boost::container::small_vector<VkWriteDescriptorSet, 4> writes;
                    const VkDescriptorImageInfo* entries{table_plan
                        ? worker_texture_table_materializer.Resolve(table_plan).data()
                        : table_data->data()};
                    for (const auto& binding : texture_table_bindings) {
                        writes.push_back({
                            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                            .pNext = nullptr,
                            .dstSet = texture_table_set,
                            .dstBinding = binding.binding,
                            .dstArrayElement = 0,
                            .descriptorCount = binding.descriptorCount,
                            .descriptorType = binding.descriptorType,
                            .pImageInfo = entries,
                            .pBufferInfo = nullptr,
                            .pTexelBufferView = nullptr,
                        });
                        entries += binding.descriptorCount;
                    }
                    device.GetLogical().UpdateDescriptorSets(writes, {});
                    texture_table_set_version = table_version;
                }
                // Binding set 0 with this layout leaves set 1 bound. Rebinding it would make
                // MoltenVK walk every resource of the tables again.
                if (bind_pipeline || texture_table_bound_set != texture_table_set) {
                    cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline_layout,
                                              1, texture_table_set, nullptr);
                    texture_table_bound_set = texture_table_set;
                }
            }
            // An empty set 0 has a layout but nothing to update
            if (!descriptor_update_template) {
                return;
            }
            if (uses_push_descriptor) {
                cmdbuf.PushDescriptorSetWithTemplateKHR(*descriptor_update_template, *pipeline_layout,
                                                        0, descriptor_data);
            } else {
                const VkDescriptorSet descriptor_set{
                    WorkerDescriptorSet(descriptor_data, descriptor_tick)};
                cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, *pipeline_layout, 0,
                                          descriptor_set, nullptr);
            }
        });
    }
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
        const size_t num_vertex_arrays = Maxwell::NumVertexArrays;
        for (size_t index = 0; index < num_vertex_arrays; ++index) {
            const u32 host_binding = vertex_input_mapping.bindings[index];
            if (host_binding >= device.GetMaxVertexInputBindings()) {
                continue;
            }
            const bool instanced = key.state.binding_divisors[index] != 0;
            const auto rate =
                instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            vertex_bindings.push_back({
                .binding = host_binding,
                .stride = key.state.vertex_strides[index],
                .inputRate = rate,
            });
            if (instanced) {
                vertex_binding_divisors.push_back({
                    .binding = host_binding,
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
                .location = vertex_input_mapping.locations[index],
                .binding = vertex_input_mapping.bindings[attribute.buffer],
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
    if (vtg) {
        // The geometry stage running as compute writes strips cut by the restart index
        switch (vtg->geometry_bindings.output_topology) {
        case Shader::OutputTopology::PointList:
            input_assembly_topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            break;
        case Shader::OutputTopology::LineStrip:
            input_assembly_topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            break;
        case Shader::OutputTopology::TriangleStrip:
            input_assembly_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        }
    }
    const bool vtg_restart{vtg && input_assembly_topology != VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    const VkPipelineInputAssemblyStateCreateInfo input_assembly_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = input_assembly_topology,
        .primitiveRestartEnable =
            vtg_restart ||
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
        device.SupportedSampleCount(MaxwellToVK::MsaaMode(key.state.msaa_mode)) !=
        VK_SAMPLE_COUNT_1_BIT;
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
        .rasterizationSamples = device.SupportedSampleCount(MaxwellToVK::MsaaMode(key.state.msaa_mode)),
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
    pipeline = Timing::Measure(Timing::current, Timing::Phase::NativeGraphics, [&] { return device.GetLogical().CreateGraphicsPipeline(
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
        *pipeline_cache); });
}

void GraphicsPipeline::Validate() {
    size_t num_images{};
    for (const auto& info : stage_infos) {
        num_images += Shader::NumDescriptors(info.texture_buffer_descriptors);
        num_images += Shader::NumDescriptors(info.image_buffer_descriptors);
        num_images += Shader::NumDescriptors(info.texture_descriptors);
        num_images += Shader::NumDescriptors(info.image_descriptors);
    }
    if (vtg) {
        for (const auto& stage : vtg->stages) {
            num_images += Shader::NumDescriptors(stage.guest_info.texture_buffer_descriptors);
            num_images += Shader::NumDescriptors(stage.guest_info.image_buffer_descriptors);
            num_images += Shader::NumDescriptors(stage.guest_info.texture_descriptors);
            num_images += Shader::NumDescriptors(stage.guest_info.image_descriptors);
        }
    }
    ASSERT(num_images == num_image_elements);
}
} // namespace Vulkan
