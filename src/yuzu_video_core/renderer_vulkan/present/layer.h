// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <array>

#include "yuzu_common/math_util.h"
#include "nxemu-module-spec/video.h"
#include "yuzu_video_core/host1x/gpu_device_memory_manager.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Layout {
struct FramebufferLayout;
}

struct PresentFilters;
struct PresentScaleSelection;
enum class PresentSharpening;

namespace Tegra {
struct FramebufferConfig;
}

namespace Service::android {
enum class PixelFormat : u32;
}

namespace Vulkan {

class AntiAliasPass;
class CatmullPass;
class Device;
class FSR;
class MemoryAllocator;
struct PresentPushConstants;
class RasterizerVulkan;
class Scheduler;

class Layer final {
public:
    explicit Layer(const Device& device, MemoryAllocator& memory_allocator, Scheduler& scheduler,
                   Tegra::MaxwellDeviceMemoryManager& device_memory, size_t image_count,
                   VkExtent2D output_size, VkDescriptorSetLayout layout,
                   const PresentFilters& filters);
    ~Layer();

    PresentScaleSelection ConfigureDraw(PresentPushConstants* out_push_constants,
                       VkDescriptorSet* out_descriptor_set, RasterizerVulkan& rasterizer,
                       VkSampler sampler, VkSampler bilinear_sampler, VkSampler area_sampler, ScalingFilter requested_filter, PresentSharpening sharpening, size_t image_index,
                       const Tegra::FramebufferConfig& framebuffer,
                       const Layout::FramebufferLayout& layout);

private:
    void CreateDescriptorPool();
    void CreateDescriptorSets(VkDescriptorSetLayout layout);
    void CreateStagingBuffer(const Tegra::FramebufferConfig& framebuffer);
    void CreateRawImages(const Tegra::FramebufferConfig& framebuffer);
    void CreateFSR(VkExtent2D output_size, bool easu_only);

    void RefreshResources(const Tegra::FramebufferConfig& framebuffer);
    void SetAntiAliasPass(VkExtent2D source_extent);
    void ReleaseRawImages();

    u64 CalculateBufferSize(const Tegra::FramebufferConfig& framebuffer) const;
    u64 GetRawImageOffset(const Tegra::FramebufferConfig& framebuffer, size_t image_index) const;

    void SetMatrixData(PresentPushConstants& data, const Layout::FramebufferLayout& layout) const;
    void SetVertexData(PresentPushConstants& data, const Layout::FramebufferLayout& layout,
                       const Common::Rectangle<f32>& crop) const;
    void UpdateDescriptorSet(VkImageView image_view, VkSampler sampler, size_t image_index);
    void UpdateRawImage(const Tegra::FramebufferConfig& framebuffer, size_t image_index);

private:
    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    const PresentFilters& filters;
    const size_t image_count{};
    const u32 max_image_dimension;
    const bool supports_catmull_intermediate;
    vk::DescriptorPool descriptor_pool{};
    vk::DescriptorSets descriptor_sets{};

    vk::Buffer buffer{};
    std::vector<vk::Image> raw_images{};
    std::vector<vk::ImageView> raw_image_views{};
    u32 raw_width{};
    u32 raw_height{};
    Service::android::PixelFormat pixel_format{};

    AntiAliasing anti_alias_setting{};
    VkExtent2D anti_alias_extent{};
    std::unique_ptr<AntiAliasPass> anti_alias{};

    std::unique_ptr<FSR> fsr{};
    std::unique_ptr<CatmullPass> catmull_pass{};
    std::array<std::unique_ptr<CatmullPass>, 2> downscale_passes{};
    std::vector<u64> resource_ticks{};
};

} // namespace Vulkan
