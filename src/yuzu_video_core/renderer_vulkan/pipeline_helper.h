// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "yuzu_common/common_types.h"
#include "yuzu_shader_recompiler/backend/spirv/emit_spirv.h"
#include "yuzu_shader_recompiler/exception.h"
#include "yuzu_shader_recompiler/shader_info.h"
#include "yuzu_video_core/renderer_vulkan/descriptor_limits.h"
#include "yuzu_video_core/renderer_vulkan/vk_texture_cache.h"
#include "yuzu_video_core/renderer_vulkan/vk_update_descriptor.h"
#include "yuzu_video_core/texture_cache/types.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

using Shader::Backend::SPIRV::NUM_TEXTURE_AND_IMAGE_SCALING_WORDS;

// Returns the primitives that reach rasterization: the output of the last stage before it.
inline Shader::OutputTopology GetRasterizationOutputTopology(
    Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology input, bool tessellation,
    bool tessellation_isolines, std::optional<Shader::OutputTopology> geometry) {
    if (geometry) {
        return *geometry;
    }
    if (tessellation) {
        return tessellation_isolines ? Shader::OutputTopology::LineStrip
                                     : Shader::OutputTopology::TriangleStrip;
    }
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    switch (input) {
    case Topology::Points:
    case Topology::Patches:
        // Patches without tessellation shaders are drawn as points.
        return Shader::OutputTopology::PointList;
    case Topology::Lines:
    case Topology::LineStrip:
    case Topology::LinesAdjacency:
    case Topology::LineStripAdjacency:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

// Triangles drawn with the line polygon mode are rasterized as lines too.
inline bool RasterizesLines(Shader::OutputTopology output,
                            Tegra::Engines::Maxwell3D::Regs::PolygonMode polygon_mode) {
    return output == Shader::OutputTopology::LineStrip ||
           (output == Shader::OutputTopology::TriangleStrip &&
            polygon_mode == Tegra::Engines::Maxwell3D::Regs::PolygonMode::Line);
}

// Direct texture descriptors are bound through the texture table set instead.
/// Smallest descriptor array handled as a bindless array
constexpr u32 LARGE_IMAGE_ARRAY_SIZE = 64;

inline u32 NumCombinedSamplers(const Shader::Info& info) {
    u32 num{};
    for (const auto& desc : info.texture_descriptors) {
        num += desc.is_direct ? 0U : desc.count;
    }
    return num;
}

/// Number of combined samplers bound in descriptor set 1 instead of set 0
inline u32 NumTableSetCombinedSamplers(const Shader::Info& info) {
    u32 num{};
    for (const auto& desc : info.texture_descriptors) {
        num += desc.in_table_set ? desc.count : 0U;
    }
    return num;
}

inline size_t NumDescriptorUpdateEntries(const Device& device, const Shader::Info& info) {
    const size_t texel_buffers{size_t{Shader::NumDescriptors(info.texture_buffer_descriptors)} +
                               Shader::NumDescriptors(info.image_buffer_descriptors)};
    return size_t{Shader::NumDescriptors(info.constant_buffer_descriptors)} +
           Shader::NumDescriptors(info.storage_buffers_descriptors) +
           texel_buffers * device.GetTexelBufferViews() + NumCombinedSamplers(info) -
           NumTableSetCombinedSamplers(info) + Shader::NumDescriptors(info.image_descriptors) +
           (info.runtime_image_write_mask_words != 0 ? 1U : 0U) +
           static_cast<size_t>(std::popcount(info.input_attachment_mask));
}

class DescriptorLayoutBuilder {
public:
    DescriptorLayoutBuilder(const Device& device_, VkShaderStageFlags storage_buffer_stages_ = 0,
                            u32 color_attachments_ = 0)
        : device{&device_}, storage_buffer_stages{storage_buffer_stages_},
          color_attachments{color_attachments_} {}

    bool CanUsePushDescriptor() const noexcept {
        return device->IsKhrPushDescriptorSupported() &&
               num_descriptors <= device->MaxPushDescriptors();
    }

    /// True when a stage indexes the texture and sampler tables with runtime handles.
    bool HasTextureTable() const noexcept {
        return !table_bindings.empty();
    }

    std::span<const VkDescriptorSetLayoutBinding> TextureTableBindings() const noexcept {
        return {table_bindings.data(), table_bindings.size()};
    }

    /// Layout of descriptor set 1, holding the texture and sampler tables.
    vk::DescriptorSetLayout CreateTextureTableSetLayout() const {
        return device->GetLogical().CreateDescriptorSetLayout({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = device->UsesUpdateAfterBindDescriptorSets()
                         ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0U,
            .bindingCount = static_cast<u32>(table_bindings.size()),
            .pBindings = table_bindings.data(),
        });
    }

    vk::DescriptorSetLayout CreateDescriptorSetLayout(bool use_push_descriptor) const {
        boost::container::small_vector<VkDescriptorSetLayoutBinding, 36> all_bindings{
            bindings.begin(), bindings.end()};
        all_bindings.insert(all_bindings.end(), table_bindings.begin(), table_bindings.end());
        // Push descriptors retain the ordinary limits even when set 1 uses
        // MoltenVK argument buffers with the larger update-after-bind limits.
        if (use_push_descriptor && device->UsesUpdateAfterBindDescriptorSets()) {
            if (const auto* violation = DescriptorLimitViolation(
                    device->DescriptorLimits(), std::span{bindings.data(), bindings.size()},
                    color_attachments)) {
                throw Shader::NotImplementedException("{}", violation);
            }
        }
        if (const auto* violation = DescriptorLimitViolation(
                (use_push_descriptor && table_bindings.empty())
                    ? device->DescriptorLimits() : device->DescriptorSetLimits(),
                std::span{all_bindings.data(), all_bindings.size()}, color_attachments)) {
            throw Shader::NotImplementedException("{}", violation);
        }
        // Set 1 needs a set 0 layout, even when it is empty.
        if (bindings.empty() && table_bindings.empty()) {
            return nullptr;
        }
        // The layout flag selects the argument-buffer resource limits. Bindings
        // retain normal update synchronization; no updates after binding are needed.
        VkDescriptorSetLayoutCreateFlags flags{};
        if (use_push_descriptor) {
            flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        } else if (device->UsesUpdateAfterBindDescriptorSets()) {
            flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        }
        return device->GetLogical().CreateDescriptorSetLayout({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = flags,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
    }

    static bool IsLargeImageArray(const VkDescriptorUpdateTemplateEntry& entry) {
        return entry.descriptorCount >= 64 &&
               (entry.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                entry.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                entry.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                entry.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER);
    }

    std::span<const VkDescriptorUpdateTemplateEntry> TemplateEntries() const {
        return entries;
    }

    vk::DescriptorUpdateTemplate CreateTemplate(VkDescriptorSetLayout descriptor_set_layout,
                                                VkPipelineLayout pipeline_layout,
                                                bool use_push_descriptor,
                                                bool exclude_large_images = false) const {
        auto template_entries = entries;
        if (exclude_large_images) {
            template_entries.erase(
                std::remove_if(template_entries.begin(), template_entries.end(), IsLargeImageArray),
                template_entries.end());
        }
        if (template_entries.empty()) {
            return nullptr;
        }
        const VkDescriptorUpdateTemplateType type =
            use_push_descriptor ? VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR
                                : VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET;
        return device->GetLogical().CreateDescriptorUpdateTemplate({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .descriptorUpdateEntryCount = static_cast<u32>(template_entries.size()),
            .pDescriptorUpdateEntries = template_entries.data(),
            .templateType = type,
            .descriptorSetLayout = descriptor_set_layout,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .pipelineLayout = pipeline_layout,
            .set = 0,
        });
    }

    vk::PipelineLayout CreatePipelineLayout(
        VkDescriptorSetLayout descriptor_set_layout,
        VkDescriptorSetLayout texture_table_set_layout = VK_NULL_HANDLE) const {
        using Shader::Backend::SPIRV::RenderAreaLayout;
        using Shader::Backend::SPIRV::RescalingLayout;
        const u32 size_offset = is_compute ? sizeof(RescalingLayout::down_factor) : 0u;
        const VkPushConstantRange range{
            .stageFlags = static_cast<VkShaderStageFlags>(
                is_compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS),
            .offset = 0,
            .size = static_cast<u32>(sizeof(RescalingLayout)) - size_offset +
                    static_cast<u32>(sizeof(RenderAreaLayout)),
        };
        const std::array set_layouts{descriptor_set_layout, texture_table_set_layout};
        const u32 num_sets{texture_table_set_layout ? 2U : descriptor_set_layout ? 1U : 0U};
        return device->GetLogical().CreatePipelineLayout({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = num_sets,
            .pSetLayouts = num_sets != 0 ? set_layouts.data() : nullptr,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &range,
        });
    }

    void Add(const Shader::Info& info, VkShaderStageFlags stage) {
        is_compute |= (stage & VK_SHADER_STAGE_COMPUTE_BIT) != 0;

        Add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, stage, info.constant_buffer_descriptors);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage, info.storage_buffers_descriptors);
        const u32 texel_buffer_views{device->GetTexelBufferViews()};
        Add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, stage, info.texture_buffer_descriptors,
            texel_buffer_views);
        Add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, stage, info.image_buffer_descriptors,
            texel_buffer_views);
        for (const auto& desc : info.texture_descriptors) {
            if (desc.is_direct) {
                AddTable(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, stage, desc.count);
            } else if (desc.in_table_set) {
                AddTable(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage, desc.count);
            } else {
                Add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage, desc.count);
            }
        }
        Add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stage, info.image_descriptors);
        if (info.runtime_sampler_count != 0) {
            AddTable(VK_DESCRIPTOR_TYPE_SAMPLER, stage, info.runtime_sampler_count);
        }
        if (info.runtime_image_write_mask_words != 0) {
            const std::array mask_descriptor{Shader::StorageBufferDescriptor{.count = 1}};
            Add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage, mask_descriptor);
        }
        for (u32 mask = info.input_attachment_mask; mask != 0; mask &= mask - 1) {
            Add(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, stage, 1U);
        }
    }

private:
    template <typename Descriptors>
    void Add(VkDescriptorType type, VkShaderStageFlags stage, const Descriptors& descriptors,
             u32 views_per_descriptor = 1) {
        if (type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && storage_buffer_stages != 0) {
            stage = storage_buffer_stages;
        }
        for (const auto& desc : descriptors) {
            Add(type, stage, desc.count * views_per_descriptor);
        }
    }

    void Add(VkDescriptorType type, VkShaderStageFlags stage, u32 count) {
        bindings.push_back({
            .binding = binding,
            .descriptorType = type,
            .descriptorCount = count,
            .stageFlags = stage,
            .pImmutableSamplers = nullptr,
        });
        entries.push_back({
            .dstBinding = binding,
            .dstArrayElement = 0,
            .descriptorCount = count,
            .descriptorType = type,
            .offset = offset,
            .stride = sizeof(DescriptorUpdateEntry),
        });
        ++binding;
        num_descriptors += count;
        offset += sizeof(DescriptorUpdateEntry) * count;
    }

    void AddTable(VkDescriptorType type, VkShaderStageFlags stage, u32 count) {
        table_bindings.push_back({
            .binding = static_cast<u32>(table_bindings.size()),
            .descriptorType = type,
            .descriptorCount = count,
            .stageFlags = stage,
            .pImmutableSamplers = nullptr,
        });
    }

    const Device* device{};
    VkShaderStageFlags storage_buffer_stages{};
    u32 color_attachments{};
    bool is_compute{};
    boost::container::small_vector<VkDescriptorSetLayoutBinding, 32> bindings;
    boost::container::small_vector<VkDescriptorUpdateTemplateEntry, 32> entries;
    boost::container::small_vector<VkDescriptorSetLayoutBinding, 4> table_bindings;
    u32 binding{};
    u32 num_descriptors{};
    size_t offset{};
};

class RescalingPushConstant {
public:
    explicit RescalingPushConstant() noexcept {}

    void PushTexture(bool is_rescaled) noexcept {
        *texture_ptr |= is_rescaled ? texture_bit : 0u;
        texture_bit <<= 1u;
        if (texture_bit == 0u) {
            texture_bit = 1u;
            ++texture_ptr;
        }
    }

    void PushImage(bool is_rescaled) noexcept {
        *image_ptr |= is_rescaled ? image_bit : 0u;
        image_bit <<= 1u;
        if (image_bit == 0u) {
            image_bit = 1u;
            ++image_ptr;
        }
    }

    const std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS>& Data() const noexcept {
        return words;
    }

private:
    std::array<u32, NUM_TEXTURE_AND_IMAGE_SCALING_WORDS> words{};
    u32* texture_ptr{words.data()};
    u32* image_ptr{words.data() + Shader::Backend::SPIRV::NUM_TEXTURE_SCALING_WORDS};
    u32 texture_bit{1u};
    u32 image_bit{1u};
};

class RenderAreaPushConstant {
public:
    bool uses_render_area{};
    std::array<f32, 4> words{};
};

inline void PushImageDescriptors(TextureCache& texture_cache,
                                 GuestDescriptorQueue& guest_descriptor_queue,
                                 const Shader::Info& info, RescalingPushConstant& rescaling,
                                 const VideoCommon::SamplerId*& samplers,
                                 const VideoCommon::ImageViewInOut*& views,
                                 std::vector<VkDescriptorImageInfo>* texture_table = nullptr) {
    const u32 num_texture_buffers = Shader::NumDescriptors(info.texture_buffer_descriptors);
    const u32 num_image_buffers = Shader::NumDescriptors(info.image_buffer_descriptors);
    views += num_texture_buffers;
    views += num_image_buffers;
    for (const auto& desc : info.texture_descriptors) {
        bool is_rescaled{};
        if (desc.is_direct) {
            // Every table entry is bound, whatever it holds. Entries that the image type of
            // the shader cannot view are replaced by the null image view.
            ImageView& null_view{texture_cache.GetImageView(VideoCommon::NULL_IMAGE_VIEW_ID)};
            for (u32 index = 0; index < desc.count; ++index) {
                ImageView& image_view{texture_cache.GetImageView((views++)->id)};
                VkImageView vk_image_view{image_view.Handle(desc.type)};
                if (vk_image_view == VK_NULL_HANDLE ||
                    VideoCore::Surface::IsPixelFormatInteger(image_view.format)) {
                    vk_image_view = null_view.Handle(desc.type);
                } else {
                    is_rescaled |= texture_cache.IsRescaling(image_view);
                }
                texture_table->push_back({
                    .sampler = VK_NULL_HANDLE,
                    .imageView = vk_image_view,
                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                });
            }
            rescaling.PushTexture(is_rescaled);
            continue;
        }
        for (u32 index = 0; index < desc.count; ++index) {
            const VideoCommon::ImageViewId image_view_id{(views++)->id};
            const VideoCommon::SamplerId sampler_id{*(samplers++)};
            ImageView& image_view{texture_cache.GetImageView(image_view_id)};
            const VkImageView vk_image_view{image_view.Handle(desc.type)};
            const Sampler& sampler{texture_cache.GetSampler(sampler_id)};
            const bool use_fallback_sampler{sampler.HasAddedAnisotropy() &&
                                            !image_view.SupportsAnisotropy()};
            const VkSampler vk_sampler{use_fallback_sampler ? sampler.HandleWithDefaultAnisotropy()
                                                            : sampler.Handle()};
            if (desc.in_table_set) {
                texture_table->push_back({
                    .sampler = vk_sampler,
                    .imageView = vk_image_view,
                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                });
            } else {
                guest_descriptor_queue.AddSampledImage(vk_image_view, vk_sampler);
            }
            is_rescaled |= texture_cache.IsRescaling(image_view);
        }
        // SPIR-V allocates one scaling bit per descriptor, including array descriptors.
        rescaling.PushTexture(is_rescaled);
    }
    for (const auto& desc : info.image_descriptors) {
        bool is_rescaled{};
        for (u32 index = 0; index < desc.count; ++index) {
            ImageView& image_view{texture_cache.GetImageView((views++)->id)};
            if (desc.is_written && !desc.is_direct) {
                texture_cache.MarkModification(image_view.image_id);
            }
            const VkImageView vk_image_view{image_view.StorageView(desc.type, desc.format)};
            guest_descriptor_queue.AddImage(vk_image_view);
            is_rescaled |= texture_cache.IsRescaling(image_view);
        }
        rescaling.PushImage(is_rescaled);
    }
    // Only graphics shaders index the sampler table with runtime handles.
    for (u32 index = 0; index < info.runtime_sampler_count; ++index) {
        const Sampler& sampler{texture_cache.GetSampler(texture_cache.GetGraphicsSamplerId(index))};
        // The sampled image is unknown here, so the guest anisotropy is kept as is.
        texture_table->push_back({
            .sampler = sampler.HasAddedAnisotropy() ? sampler.HandleWithDefaultAnisotropy()
                                                    : sampler.Handle(),
            .imageView = VK_NULL_HANDLE,
            .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });
    }
}

} // namespace Vulkan
