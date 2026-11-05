// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/renderer_vulkan/present/catmull_pass.h"
#include "yuzu_video_core/host_shaders/present_catmull_rom_frag_spv.h"
#include "yuzu_video_core/host_shaders/present_area_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_frag_spv.h"
#include "yuzu_video_core/host_shaders/vulkan_present_vert_spv.h"
#include "yuzu_video_core/renderer_vulkan/present/present_push_constants.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/vk_shader_util.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
CatmullPass::CatmullPass(const Device &device_, MemoryAllocator &allocator, size_t image_count,
                         VkExtent2D output_extent, ScalingFilter filter)
    : device(device_), extent(output_extent),
      padding(filter == ScalingFilter::CatmullRom ? 1U : 0U) {
    extent.width += 2 * padding;
    extent.height += 2 * padding;
    sampler = padding ? CreateWrappedSampler(device)
                      : CreateBilinearSampler(device, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    descriptor_layout =
        CreateWrappedDescriptorSetLayout(device, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER});
    descriptor_pool = CreateWrappedDescriptorPool(device, image_count, image_count);
    const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof(PresentPushConstants)};
    pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &range,
    });
    vertex = BuildShader(device, VULKAN_PRESENT_VERT_SPV);
    fragment = filter == ScalingFilter::Area ? BuildShader(device, PRESENT_AREA_FRAG_SPV)
        : filter == ScalingFilter::Bilinear ? BuildShader(device, VULKAN_PRESENT_FRAG_SPV)
                                          : BuildShader(device, PRESENT_CATMULL_ROM_FRAG_SPV);
    // FP32 arithmetic; UNORM16 storage preserves SDR precision at eight bytes per pixel.
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const auto format =
        (device.GetPhysical().GetFormatProperties(VK_FORMAT_R16G16B16A16_UNORM).optimalTilingFeatures & required) == required
            ? VK_FORMAT_R16G16B16A16_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    renderpass = CreateWrappedRenderPass(device, format);
    pipeline =
        CreateWrappedPipeline(device, renderpass, pipeline_layout, std::tie(vertex, fragment));
    images.reserve(image_count);
    for (size_t i = 0; i < image_count; ++i) {
        auto &image = images.emplace_back();
        image.image = CreateWrappedImage(allocator, extent, format);
        image.view = CreateWrappedImageView(device, image.image, format);
        image.framebuffer = CreateWrappedFramebuffer(device, renderpass, image.view, extent);
        image.sets = CreateWrappedDescriptorSets(descriptor_pool, {*descriptor_layout});
    }
}

Common::Rectangle<f32> CatmullPass::OutputCrop() const {
    const float u = float(padding) / extent.width, v = float(padding) / extent.height;
    return {u, v, 1.f - u, 1.f - v};
}

VkImageView CatmullPass::Draw(Scheduler &scheduler, size_t image_index, VkImage source,
                              VkImageView source_view, const Common::Rectangle<f32> &crop,
                              bool fast, const PresentPushConstants* constants) {
    auto &image = images[image_index];
    const VkDescriptorImageInfo info{*sampler, source_view, VK_IMAGE_LAYOUT_GENERAL};
    const VkWriteDescriptorSet write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = image.sets[0],
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    device.GetLogical().UpdateDescriptorSets(write, {});
    PresentPushConstants pc{};
    pc.transform = MakeOrthographicTransform(float(extent.width), float(extent.height));
    const float du = padding * (crop.right - crop.left) / (extent.width - 2 * padding);
    const float dv = padding * (crop.bottom - crop.top) / (extent.height - 2 * padding);
    const float w = float(extent.width), h = float(extent.height);
    pc.vertices = {ScreenRectVertex(0, 0, crop.left - du, crop.top - dv),
                   ScreenRectVertex(w, 0, crop.right + du, crop.top - dv),
                   ScreenRectVertex(0, h, crop.left - du, crop.bottom + dv),
                   ScreenRectVertex(w, h, crop.right + du, crop.bottom + dv)};
    if (constants) {
        pc.filter_constants = constants->filter_constants;
    } else {
        pc.filter_constants[1] = fast ? 2U : 0U;
    }
    const VkImage output = *image.image;
    const VkFramebuffer framebuffer = *image.framebuffer;
    const VkDescriptorSet set = image.sets[0];
    const VkPipeline draw_pipeline = *pipeline;
    const VkPipelineLayout layout = *pipeline_layout;
    const VkRenderPass pass = *renderpass;
    const VkExtent2D render_extent = extent;
    const bool initialized = image.initialized;
    image.initialized = true;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        TransitionImageLayout(cmdbuf, source, VK_IMAGE_LAYOUT_GENERAL);
        TransitionImageLayout(cmdbuf, output, VK_IMAGE_LAYOUT_GENERAL,
                              initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
        BeginRenderPass(cmdbuf, pass, framebuffer, render_extent);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, draw_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, set, {});
        cmdbuf.PushConstants(layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, pc);
        cmdbuf.Draw(4, 1, 0, 0);
        cmdbuf.EndRenderPass();
        TransitionImageLayout(cmdbuf, output, VK_IMAGE_LAYOUT_GENERAL);
    });
    return *image.view;
}
} // namespace Vulkan
