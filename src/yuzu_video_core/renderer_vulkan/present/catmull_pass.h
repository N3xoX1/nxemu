// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nxemu-module-spec/video.h"
#include "yuzu_common/math_util.h"
#include "yuzu_video_core/vulkan_common/vulkan_memory_allocator.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
class Scheduler;
struct PresentPushConstants;
// Resample once before sharpening. Catmull includes a one-output-pixel halo;
// downscalers produce the final-sized image with clamped output boundaries.
// Per-swapchain images/descriptor sets are reused after Layer's resource tick wait.
class CatmullPass {
  public:
    CatmullPass(const Device &device, MemoryAllocator &allocator, size_t image_count,
                VkExtent2D output_extent, ScalingFilter filter = ScalingFilter::CatmullRom);
    VkImageView Draw(Scheduler &scheduler, size_t image_index, VkImage source,
                     VkImageView source_view, const Common::Rectangle<f32> &crop, bool fast,
                     const PresentPushConstants* constants = nullptr);
    Common::Rectangle<f32> OutputCrop() const;

  private:
    const Device &device;
    VkExtent2D extent;
    u32 padding;
    vk::Sampler sampler;
    vk::DescriptorSetLayout descriptor_layout;
    vk::DescriptorPool descriptor_pool;
    vk::PipelineLayout pipeline_layout;
    vk::ShaderModule vertex, fragment;
    vk::RenderPass renderpass;
    vk::Pipeline pipeline;
    struct Image {
        vk::Image image;
        vk::ImageView view;
        vk::Framebuffer framebuffer;
        vk::DescriptorSets sets;
        bool initialized{};
    };
    std::vector<Image> images;
};
} // namespace Vulkan
