// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/common_types.h"
#include "yuzu_common/div_ceil.h"
#include "yuzu_common/settings.h"
#include "video_settings.h"

#include "yuzu_video_core/fsr.h"
#include "yuzu_video_core/host_shaders/vulkan_fidelityfx_fsr_easu_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_fidelityfx_fsr_easu_fp32_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_fidelityfx_fsr_rcas_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_fidelityfx_fsr_rcas_fp32_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_fidelityfx_fsr_vert_spv.h"
#include "yuzu_video_core/renderer_vulkan/present/fsr.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
using namespace FSR;

using PushConstants = std::array<u32, 4 * 4>;

FSR::FSR(const Device& device, MemoryAllocator& memory_allocator, size_t image_count,
         VkExtent2D extent, bool easu_only)
    : m_device{device}, m_memory_allocator{memory_allocator},
      m_image_count{image_count}, m_extent{extent}, m_easu_only{easu_only} {

    CreateImages();
    CreateRenderPasses();
    CreateSampler();
    CreateShaders();
    CreateDescriptorPool();
    CreateDescriptorSetLayout();
    CreateDescriptorSets();
    CreatePipelineLayouts();
    CreatePipelines();
}

void FSR::CreateImages() {
    m_dynamic_images.resize(m_image_count);
    for (auto& images : m_dynamic_images) {
        images.images[Easu] =
            CreateWrappedImage(m_memory_allocator, m_extent, VK_FORMAT_R16G16B16A16_SFLOAT);
        images.image_views[Easu] =
            CreateWrappedImageView(m_device, images.images[Easu], VK_FORMAT_R16G16B16A16_SFLOAT);
        if (!m_easu_only) {
            images.images[Rcas] =
                CreateWrappedImage(m_memory_allocator, m_extent, VK_FORMAT_R16G16B16A16_SFLOAT);
            images.image_views[Rcas] = CreateWrappedImageView(
                m_device, images.images[Rcas], VK_FORMAT_R16G16B16A16_SFLOAT);
        }
    }
}

void FSR::CreateRenderPasses() {
    m_renderpass = CreateWrappedRenderPass(m_device, VK_FORMAT_R16G16B16A16_SFLOAT);

    for (auto& images : m_dynamic_images) {
        images.framebuffers[Easu] =
            CreateWrappedFramebuffer(m_device, m_renderpass, images.image_views[Easu], m_extent);
        if (!m_easu_only) {
            images.framebuffers[Rcas] = CreateWrappedFramebuffer(
                m_device, m_renderpass, images.image_views[Rcas], m_extent);
        }
    }
}

void FSR::CreateSampler() {
    m_sampler = CreateBilinearSampler(m_device);
}

void FSR::CreateShaders() {
    m_vert_shader = BuildShader(m_device, VULKAN_FIDELITYFX_FSR_VERT_SPV);

    if (m_device.IsFloat16Supported() && m_device.IsShaderInt16Supported()) {
        m_easu_shader = BuildShader(m_device, VULKAN_FIDELITYFX_FSR_EASU_FP16_FRAG_SPV);
        if (!m_easu_only) {
            m_rcas_shader = BuildShader(m_device, VULKAN_FIDELITYFX_FSR_RCAS_FP16_FRAG_SPV);
        }
    } else {
        m_easu_shader = BuildShader(m_device, VULKAN_FIDELITYFX_FSR_EASU_FP32_FRAG_SPV);
        if (!m_easu_only) {
            m_rcas_shader = BuildShader(m_device, VULKAN_FIDELITYFX_FSR_RCAS_FP32_FRAG_SPV);
        }
    }
}

void FSR::CreateDescriptorPool() {
    const size_t descriptors = (m_easu_only ? 1 : 2) * m_image_count;
    m_descriptor_pool = CreateWrappedDescriptorPool(m_device, descriptors, descriptors);
}

void FSR::CreateDescriptorSetLayout() {
    m_descriptor_set_layout =
        CreateWrappedDescriptorSetLayout(m_device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
}

void FSR::CreateDescriptorSets() {
    std::vector<VkDescriptorSetLayout> layouts(m_easu_only ? 1 : MaxFsrStage,
                                               *m_descriptor_set_layout);

    for (auto& images : m_dynamic_images) {
        images.descriptor_sets = CreateWrappedDescriptorSets(m_descriptor_pool, layouts);
    }
}

void FSR::CreatePipelineLayouts() {
    const VkPushConstantRange range{
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    VkPipelineLayoutCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = m_descriptor_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    };

    m_pipeline_layout = m_device.GetLogical().CreatePipelineLayout(ci);
}

void FSR::CreatePipelines() {
    m_easu_pipeline = CreateWrappedPipeline(m_device, m_renderpass, m_pipeline_layout,
                                            std::tie(m_vert_shader, m_easu_shader));
    if (!m_easu_only) {
        m_rcas_pipeline = CreateWrappedPipeline(m_device, m_renderpass, m_pipeline_layout,
                                                std::tie(m_vert_shader, m_rcas_shader));
    }
}

void FSR::UpdateDescriptorSets(VkImageView image_view, size_t image_index, bool native_rcas) {
    Images& images = m_dynamic_images[image_index];
    std::vector<VkDescriptorImageInfo> image_infos;
    std::vector<VkWriteDescriptorSet> updates;
    image_infos.reserve(2);

    if (!native_rcas) {
        updates.push_back(CreateWriteDescriptorSet(image_infos, *m_sampler, image_view,
                                                   images.descriptor_sets[Easu], 0));
    }
    if (!m_easu_only) {
        updates.push_back(CreateWriteDescriptorSet(
            image_infos, *m_sampler, native_rcas ? image_view : *images.image_views[Easu],
            images.descriptor_sets[Rcas], 0));
    }

    m_device.GetLogical().UpdateDescriptorSets(updates, {});
}

void FSR::UploadImages(Scheduler& scheduler) {
    if (m_images_ready) {
        return;
    }

    scheduler.Record([&](vk::CommandBuffer cmdbuf) {
        for (auto& image : m_dynamic_images) {
            ClearColorImage(cmdbuf, *image.images[Easu]);
            if (!m_easu_only) {
                ClearColorImage(cmdbuf, *image.images[Rcas]);
            }
        }
    });
    scheduler.Finish();

    m_images_ready = true;
}

VkImageView FSR::Draw(Scheduler& scheduler, size_t image_index, VkImage source_image,
                      VkImageView source_image_view, VkExtent2D input_image_extent,
                      const Common::Rectangle<f32>& crop_rect) {
    const bool native_rcas = !VideoCore::LegacyFsr() && CanUseNativeRcas(input_image_extent.width, input_image_extent.height,
                                              crop_rect, m_extent.width, m_extent.height);
    if (m_easu_only && native_rcas) {
        return source_image_view;
    }
    Images& images = m_dynamic_images[image_index];

    VkImage easu_image = *images.images[Easu];
    VkImage rcas_image = m_easu_only ? VK_NULL_HANDLE : *images.images[Rcas];
    VkDescriptorSet easu_descriptor_set = images.descriptor_sets[Easu];
    VkDescriptorSet rcas_descriptor_set = m_easu_only ? VK_NULL_HANDLE : images.descriptor_sets[Rcas];
    VkFramebuffer easu_framebuffer = *images.framebuffers[Easu];
    VkFramebuffer rcas_framebuffer = m_easu_only ? VK_NULL_HANDLE : *images.framebuffers[Rcas];
    VkPipeline easu_pipeline = *m_easu_pipeline;
    VkPipeline rcas_pipeline = m_easu_only ? VK_NULL_HANDLE : *m_rcas_pipeline;
    VkPipelineLayout pipeline_layout = *m_pipeline_layout;
    VkRenderPass renderpass = *m_renderpass;
    VkExtent2D extent = m_extent;
    const bool easu_only = m_easu_only;

    const f32 input_image_width = static_cast<f32>(input_image_extent.width);
    const f32 input_image_height = static_cast<f32>(input_image_extent.height);
    const f32 output_image_width = static_cast<f32>(extent.width);
    const f32 output_image_height = static_cast<f32>(extent.height);
    const f32 viewport_width = (crop_rect.right - crop_rect.left) * input_image_width;
    const f32 viewport_x = crop_rect.left * input_image_width;
    const f32 viewport_height = (crop_rect.bottom - crop_rect.top) * input_image_height;
    const f32 viewport_y = crop_rect.top * input_image_height;

    PushConstants easu_con{};
    PushConstants rcas_con{};
    if (!native_rcas) {
        FsrEasuConOffset(easu_con.data() + 0, easu_con.data() + 4, easu_con.data() + 8,
                         easu_con.data() + 12, viewport_width, viewport_height, input_image_width,
                         input_image_height, output_image_width, output_image_height, viewport_x,
                         viewport_y);
    }

    if (!m_easu_only) {
        const auto strength = SharpeningStrength::FromLegacyRcas(
            videoSettings.scaling_filter == ScalingFilter::FsrEasu
                ? FsrSharpness::StoredMaximum : videoSettings.fsr_sharpening_slider);
        FsrRcasConFromStrength(rcas_con.data(), strength);
    }
    UploadImages(scheduler);
    UpdateDescriptorSets(source_image_view, image_index, native_rcas);

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        TransitionImageLayout(cmdbuf, source_image, VK_IMAGE_LAYOUT_GENERAL);
        if (!native_rcas) {
            TransitionImageLayout(cmdbuf, easu_image, VK_IMAGE_LAYOUT_GENERAL);
            BeginRenderPass(cmdbuf, renderpass, easu_framebuffer, extent);
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, easu_pipeline);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0,
                                      easu_descriptor_set, {});
            cmdbuf.PushConstants(pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, easu_con);
            cmdbuf.Draw(3, 1, 0, 0);
            cmdbuf.EndRenderPass();
            TransitionImageLayout(cmdbuf, easu_image, VK_IMAGE_LAYOUT_GENERAL);
        }

        if (!easu_only) {
            TransitionImageLayout(cmdbuf, rcas_image, VK_IMAGE_LAYOUT_GENERAL);
            BeginRenderPass(cmdbuf, renderpass, rcas_framebuffer, extent);
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, rcas_pipeline);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0,
                                      rcas_descriptor_set, {});
            cmdbuf.PushConstants(pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, rcas_con);
            cmdbuf.Draw(3, 1, 0, 0);
            cmdbuf.EndRenderPass();
            TransitionImageLayout(cmdbuf, rcas_image, VK_IMAGE_LAYOUT_GENERAL);
        }
    });

    return m_easu_only ? *images.image_views[Easu] : *images.image_views[Rcas];
}

} // namespace Vulkan
