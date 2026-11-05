// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <list>
#include "nxemu-module-spec/video.h"

#include "yuzu_common/math_util.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout {
struct FramebufferLayout;
}

namespace Tegra {
struct FramebufferConfig;
}
enum class PresentSharpening;

namespace Vulkan {

class Device;
struct Frame;
class Layer;
class Scheduler;
class RasterizerVulkan;

class WindowAdaptPass final {
public:
    explicit WindowAdaptPass(const Device& device, VkFormat frame_format, vk::Sampler&& sampler,
                             vk::ShaderModule&& fragment_shader, bool use_rcas = false,
                             bool use_catmull = false, bool use_cas = false);
    ~WindowAdaptPass();

    void Draw(RasterizerVulkan& rasterizer, Scheduler& scheduler, size_t image_index,
              std::list<Layer>& layers, std::span<const Tegra::FramebufferConfig> configs,
              const Layout::FramebufferLayout& layout, Frame* dst, ScalingFilter requested_filter,
              PresentSharpening sharpening);

    VkDescriptorSetLayout GetDescriptorSetLayout();
    VkRenderPass GetRenderPass();

private:
    struct FallbackPipelines {
        vk::Sampler sampler;
        vk::ShaderModule shader;
        vk::Pipeline opaque, premultiplied, coverage;
    };
    void CreateFallback(FallbackPipelines& target, ScalingFilter filter);
    FallbackPipelines bilinear_fallback, area_fallback, cas_fallback, rcas_fallback;
    void CreateDescriptorSetLayout();
    void CreatePipelineLayout();
    void CreateVertexShader();
    void CreateRenderPass(VkFormat frame_format);
    void CreatePipelines();

private:
    const Device& device;
    const bool use_rcas;
    const bool use_catmull;
    const bool use_cas;
    vk::DescriptorSetLayout descriptor_set_layout;
    vk::PipelineLayout pipeline_layout;
    vk::Sampler sampler;
    vk::ShaderModule vertex_shader;
    vk::ShaderModule fragment_shader;
    vk::RenderPass render_pass;
    vk::Pipeline opaque_pipeline;
    vk::Pipeline premultiplied_pipeline;
    vk::Pipeline coverage_pipeline;
};

} // namespace Vulkan
