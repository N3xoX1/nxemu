// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <boost/container/small_vector.hpp>

#include "frontend/framebuffer_layout.h"
#include "yuzu_video_core/fsr.h"
#include "yuzu_video_core/fsr_diagnostics.h"
#include "yuzu_video_core/framebuffer_config.h"
#include "yuzu_video_core/host_shaders/vulkan_present_vert_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_frag_spv.h"
#include "yuzu_video_core/host_shaders/present_area_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_cas_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_cas_quad_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_rcas_fp16_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_rcas_fp32_frag_spv.h"
#include "yuzu_video_core/present.h"
#include "yuzu_video_core/present_sharpening.h"
#include "yuzu_video_core/renderer_vulkan/present/layer.h"
#include "yuzu_video_core/renderer_vulkan/present/present_push_constants.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/present/window_adapt_pass.h"
#include "yuzu_video_core/renderer_vulkan/vk_present_manager.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_settings.h"

namespace Vulkan {

WindowAdaptPass::WindowAdaptPass(const Device& device_, VkFormat frame_format,
                                 vk::Sampler&& sampler_, vk::ShaderModule&& fragment_shader_,
                                 bool use_rcas_, bool use_catmull_, bool use_cas_)
    : device(device_), use_rcas(use_rcas_), use_catmull(use_catmull_), use_cas(use_cas_), sampler(std::move(sampler_)),
      fragment_shader(std::move(fragment_shader_)) {
    CreateDescriptorSetLayout();
    CreatePipelineLayout();
    CreateVertexShader();
    CreateRenderPass(frame_format);
    CreatePipelines();
}

WindowAdaptPass::~WindowAdaptPass() = default;

void WindowAdaptPass::Draw(RasterizerVulkan& rasterizer, Scheduler& scheduler, size_t image_index,
                           std::list<Layer>& layers,
                           std::span<const Tegra::FramebufferConfig> configs,
                           const Layout::FramebufferLayout& layout, Frame* dst, ScalingFilter requested_filter,
                           PresentSharpening sharpening) {

    const VkFramebuffer host_framebuffer{*dst->framebuffer};
    const VkRenderPass renderpass{*render_pass};
    const VkPipelineLayout graphics_pipeline_layout{*pipeline_layout};
    const VkExtent2D render_area{
        .width = dst->width,
        .height = dst->height,
    };

    if (!bilinear_fallback.sampler) {
        bilinear_fallback.sampler =
            CreateBilinearSampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }
    if (!area_fallback.sampler && VideoCore::fsr_frame_state.downscale_filter == 1) {
        area_fallback.sampler =
            CreateBilinearSampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }
    const size_t layer_count = configs.size();
    struct PreparedLayer {
        PresentPushConstants constants{};
        VkDescriptorSet descriptor{};
        VkPipeline pipeline{};
    };
    // One layer is the common case. Keep its data inline, and move it into the
    // deferred command so it stays alive without six vector allocations/copies.
    boost::container::small_vector<PreparedLayer, 1> prepared(layer_count);

    std::array<u32, 4> rcas_constants{};
    if (use_rcas || sharpening == PresentSharpening::Rcas) {
        ::FSR::FsrRcasConFromStrength(rcas_constants.data(),
                          SharpeningStrength::FromLegacyRcas(VideoCore::fsr_frame_state.sharpness_stored));
    }

    auto layer_it = layers.begin();
    for (size_t i = 0; i < layer_count; i++) {
        switch (configs[i].blending) {
        case Tegra::BlendMode::Opaque:
        default:
            prepared[i].pipeline = *opaque_pipeline;
            break;
        case Tegra::BlendMode::Premultiplied:
            prepared[i].pipeline = *premultiplied_pipeline;
            break;
        case Tegra::BlendMode::Coverage:
            prepared[i].pipeline = *coverage_pipeline;
            break;
        }

        const auto effective = layer_it->ConfigureDraw(&prepared[i].constants, &prepared[i].descriptor,
            rasterizer, *sampler, *bilinear_fallback.sampler, *area_fallback.sampler,
            requested_filter, sharpening, image_index, configs[i], layout);
        const bool sharpen_only = effective.mode != PresentScaleMode::Upscale &&
                                  sharpening != PresentSharpening::None;
        const auto final_filter = sharpen_only
            ? (sharpening == PresentSharpening::Cas ? ScalingFilter::CasBilinear : ScalingFilter::Fsr)
            : effective.filter;
        FallbackPipelines* fallback = final_filter == ScalingFilter::Area ? &area_fallback
            : final_filter == ScalingFilter::Bilinear ? &bilinear_fallback
            : sharpen_only && sharpening == PresentSharpening::Cas ? &cas_fallback
            : sharpen_only && sharpening == PresentSharpening::Rcas ? &rcas_fallback : nullptr;
        if (fallback) {
            CreateFallback(*fallback, final_filter);
            switch (configs[i].blending) {
            case Tegra::BlendMode::Premultiplied: prepared[i].pipeline = *fallback->premultiplied; break;
            case Tegra::BlendMode::Coverage: prepared[i].pipeline = *fallback->coverage; break;
            default: prepared[i].pipeline = *fallback->opaque; break;
            }
        }
        if ((use_rcas && !fallback) || fallback == &rcas_fallback) {
            std::copy(rcas_constants.begin(), rcas_constants.end(),
                      prepared[i].constants.filter_constants.begin());
        }
        if ((use_cas && !fallback) || fallback == &cas_fallback) {
            prepared[i].constants.filter_constants[0] = std::bit_cast<u32>(
                SharpeningStrength::FromPercent(VideoCore::fsr_frame_state.cas_strength).CasUniform());
        }
        layer_it++;
    }

    scheduler.Record([=, prepared = std::move(prepared)](vk::CommandBuffer cmdbuf) {
        const f32 bg_red = videoSettings.bg_red / 255.0f;
        const f32 bg_green = videoSettings.bg_green / 255.0f;
        const f32 bg_blue = videoSettings.bg_blue / 255.0f;
        const VkClearAttachment clear_attachment{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .colorAttachment = 0,
            .clearValue =
                {
                    .color = {.float32 = {bg_red, bg_green, bg_blue, 1.0f}},
                },
        };
        const VkClearRect clear_rect{
            .rect =
                {
                    .offset = {0, 0},
                    .extent = render_area,
                },
            .baseArrayLayer = 0,
            .layerCount = 1,
        };

        BeginRenderPass(cmdbuf, renderpass, host_framebuffer, render_area);
        cmdbuf.ClearAttachments({clear_attachment}, {clear_rect});

        for (size_t i = 0; i < layer_count; i++) {
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, prepared[i].pipeline);
            cmdbuf.PushConstants(graphics_pipeline_layout,
                                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                 prepared[i].constants);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline_layout, 0,
                                      prepared[i].descriptor, {});
            cmdbuf.Draw(4, 1, 0, 0);
        }

        cmdbuf.EndRenderPass();
    });
}

VkDescriptorSetLayout WindowAdaptPass::GetDescriptorSetLayout() {
    return *descriptor_set_layout;
}

VkRenderPass WindowAdaptPass::GetRenderPass() {
    return *render_pass;
}

void WindowAdaptPass::CreateDescriptorSetLayout() {
    descriptor_set_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
}

void WindowAdaptPass::CreatePipelineLayout() {
    const VkPushConstantRange range{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PresentPushConstants),
    };

    pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    });
}

void WindowAdaptPass::CreateVertexShader() {
    vertex_shader = BuildShader(device, VULKAN_PRESENT_VERT_SPV);
}

void WindowAdaptPass::CreateRenderPass(VkFormat frame_format) {
    render_pass = CreateWrappedRenderPass(device, frame_format, VK_IMAGE_LAYOUT_UNDEFINED);
}

void WindowAdaptPass::CreateFallback(FallbackPipelines& target, ScalingFilter filter) {
    if (target.shader) return;
    if (!target.sampler) {
        target.sampler = CreateBilinearSampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    }
    switch (filter) {
    case ScalingFilter::Area:
        target.shader = BuildShader(device, PRESENT_AREA_FRAG_SPV);
        break;
    case ScalingFilter::CasBilinear: {
        const bool quad = device.IsSubgroupStageSupported(VK_SHADER_STAGE_FRAGMENT_BIT) &&
            device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_BASIC_BIT) &&
            device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_QUAD_BIT);
        target.shader = quad ? BuildShader(device, VULKAN_PRESENT_CAS_QUAD_FRAG_SPV)
                             : BuildShader(device, VULKAN_PRESENT_CAS_FRAG_SPV);
        break;
    }
    case ScalingFilter::Fsr:
        target.shader = device.IsFloat16Supported() && device.IsShaderInt16Supported()
            ? BuildShader(device, VULKAN_PRESENT_RCAS_FP16_FRAG_SPV)
            : BuildShader(device, VULKAN_PRESENT_RCAS_FP32_FRAG_SPV);
        break;
    default:
        target.shader = BuildShader(device, VULKAN_PRESENT_FRAG_SPV);
        break;
    }
    target.opaque = CreateWrappedPipeline(device, render_pass, pipeline_layout,
        std::tie(vertex_shader, target.shader));
    target.premultiplied = CreateWrappedPremultipliedBlendingPipeline(device, render_pass,
        pipeline_layout, std::tie(vertex_shader, target.shader));
    target.coverage = CreateWrappedCoverageBlendingPipeline(device, render_pass,
        pipeline_layout, std::tie(vertex_shader, target.shader));
}

void WindowAdaptPass::CreatePipelines() {
    opaque_pipeline = CreateWrappedPipeline(device, render_pass, pipeline_layout,
                                            std::tie(vertex_shader, fragment_shader));
    premultiplied_pipeline = CreateWrappedPremultipliedBlendingPipeline(
        device, render_pass, pipeline_layout, std::tie(vertex_shader, fragment_shader));
    coverage_pipeline = CreateWrappedCoverageBlendingPipeline(
        device, render_pass, pipeline_layout, std::tie(vertex_shader, fragment_shader));
}

} // namespace Vulkan
