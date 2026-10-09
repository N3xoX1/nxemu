// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <span>

#include "yuzu_video_core/engines/fermi_2d.h"
#include "yuzu_video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/texture_cache/types.h"
#include "yuzu_video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

using VideoCommon::Extent3D;
using VideoCommon::Offset2D;
using VideoCommon::Region2D;

class Device;
class Framebuffer;
class ImageView;
class RenderPassCache;
class StateTracker;
class Scheduler;

struct BlitImagePipelineKey {
    constexpr auto operator<=>(const BlitImagePipelineKey&) const noexcept = default;

    VkRenderPass renderpass;
    Tegra::Engines::Fermi2D::Operation operation;
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    u8 color_attachment_count{1};
    u8 clear_color_mask{0xf};
    // 0: float, 1: signed integer, 2: unsigned integer.
    u8 clear_color_type{};
    bool masked_clear{};
};

struct BlitDepthStencilPipelineKey {
    constexpr auto operator<=>(const BlitDepthStencilPipelineKey&) const noexcept = default;

    VkRenderPass renderpass;
    bool depth_clear;
    u8 stencil_mask;
    u32 stencil_compare_mask;
    u32 stencil_ref;
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
};

enum class MSAACopyFormatClass : u32 {
    Float,
    SignedInteger,
    UnsignedInteger,
};

struct MSAACopyPipelineKey {
    constexpr auto operator<=>(const MSAACopyPipelineKey&) const noexcept = default;

    VkRenderPass renderpass;
    VkSampleCountFlagBits samples;
    MSAACopyFormatClass format_class;
    bool packed_depth_stencil{};
};

struct BlitMSAAPipelineKey {
    constexpr auto operator<=>(const BlitMSAAPipelineKey&) const noexcept = default;

    VkRenderPass renderpass;
    VkSampleCountFlagBits samples;
    MSAACopyFormatClass format_class;
    bool linear_filter{};
    VkSampleCountFlagBits source_samples{};
};

class BlitImageHelper {
public:
    explicit BlitImageHelper(const Device& device, Scheduler& scheduler,
                             StateTracker& state_tracker, DescriptorPool& descriptor_pool);
    ~BlitImageHelper();

    void TickFrame();

    void BlitColor(const Framebuffer* dst_framebuffer, VkImageView src_image_view,
                   const Region2D& dst_region, const Region2D& src_region,
                   Tegra::Engines::Fermi2D::Filter filter,
                   Tegra::Engines::Fermi2D::Operation operation);

    void BlitColor(const Framebuffer* dst_framebuffer, VkImageView src_image_view,
                   VkImage src_image, VkSampler src_sampler, const Region2D& dst_region,
                   const Region2D& src_region, const Extent3D& src_size);

    void BlitDepthStencil(const Framebuffer* dst_framebuffer, VkImageView src_depth_view,
                          VkImageView src_stencil_view, const Region2D& dst_region,
                          const Region2D& src_region, Tegra::Engines::Fermi2D::Filter filter,
                          Tegra::Engines::Fermi2D::Operation operation);

    /// Blits a multisampled color image with a shader. The destination can have any sample count.
    void BlitColorMSAA(const Framebuffer* dst_framebuffer, const ImageView& src_image_view,
                       const Region2D& dst_region, const Region2D& src_region,
                       Tegra::Engines::Fermi2D::Filter filter);

    /// Blits a multisampled depth image with a shader. Stencil needs shader stencil export.
    void BlitDepthStencilMSAA(const Framebuffer* dst_framebuffer, ImageView& src_image_view,
                              const Region2D& dst_region, const Region2D& src_region);

    bool TryResolveDepthStencilMSAA(ImageView & dst, ImageView & src, const Region2D & region);

    void ConvertD32ToR32(const Framebuffer* dst_framebuffer, const ImageView& src_image_view);

    void ConvertR32ToD32(const Framebuffer* dst_framebuffer, const ImageView& src_image_view);

    void ConvertD16ToR16(const Framebuffer* dst_framebuffer, const ImageView& src_image_view);

    void ConvertR16ToD16(const Framebuffer* dst_framebuffer, const ImageView& src_image_view);

    void ConvertABGR8ToD24S8(const Framebuffer* dst_framebuffer, const ImageView& src_image_view);

    void ConvertABGR8ToS8D24(const Framebuffer* dst_framebuffer, const ImageView& src_image_view);

    void ConvertABGR8ToD32F(const Framebuffer* dst_framebuffer, const ImageView& src_image_view);

    void ConvertD32FToABGR8(const Framebuffer* dst_framebuffer, ImageView& src_image_view);

    void ConvertD24S8ToABGR8(const Framebuffer* dst_framebuffer, ImageView& src_image_view);

    void ConvertS8D24ToABGR8(const Framebuffer* dst_framebuffer, ImageView& src_image_view);

    void ClearColor(const Framebuffer* dst_framebuffer, u8 color_mask,
                    const std::array<f32, 4>& clear_color, const Region2D& dst_region);

    void ClearColor(const Framebuffer* dst_framebuffer, u8 color_mask,
                    const VkClearColorValue& clear_color, VideoCore::Surface::PixelFormat format,
                    const Region2D& dst_region);

    void ClearDepthStencil(const Framebuffer* dst_framebuffer, bool depth_clear, f32 clear_depth,
                           u8 stencil_mask, u32 stencil_ref, u32 stencil_compare_mask,
                           const Region2D& dst_region);

    void CopyToMSAA(RenderPassCache& render_pass_cache, VkImage dst_image,
                    VideoCore::Surface::PixelFormat format, VkImage src_image, u32 num_samples,
                    std::span<const VideoCommon::ImageCopy> copies);

    void CopyToMSAADepth(RenderPassCache& render_pass_cache, VkImage dst_image,
                         VideoCore::Surface::PixelFormat format, VkImage src_image, u32 num_samples,
                         std::span<const VideoCommon::ImageCopy> copies, bool copy_stencil);

    void CopyPackedDepthStencilToMSAA(RenderPassCache& render_pass_cache, VkImage dst_image,
                                      VideoCore::Surface::PixelFormat format, VkImage src_image,
                                      VkFormat src_format, u32 num_samples,
                                      std::span<const VideoCommon::ImageCopy> copies,
                                      s32 packed_format, bool copy_stencil);

private:
    struct MSAACopyAspectInfo {
        VkImageAspectFlags src_view_aspect;
        VkImageAspectFlags attachment_aspect;
        VkImageAspectFlags src_barrier_aspect;
        VkImageAspectFlags dst_barrier_aspect;
        VkAccessFlags pre_src_access;
        VkAccessFlags pre_dst_access;
        VkAccessFlags pre_src_dst_access;
        VkAccessFlags pre_dst_dst_access;
        VkPipelineStageFlags pre_src_stages;
        VkPipelineStageFlags pre_dst_stages;
        VkAccessFlags post_src_access;
        VkAccessFlags post_dst_access;
        VkPipelineStageFlags post_src_stages;
        VkPipelineStageFlags post_dst_stages;
    };

    void CopyToMSAAImpl(VkRenderPass renderpass, VkPipeline pipeline, VkPipelineLayout layout,
                        VkImage dst_image, VkFormat dst_format, VkImage src_image,
                        VkFormat src_format, s32 scale_x, s32 scale_y,
                        std::span<const VideoCommon::ImageCopy> copies,
                        const MSAACopyAspectInfo& aspect_info, bool copy_stencil,
                        s32 packed_format = 0);

    void Convert(VkPipeline pipeline, const Framebuffer* dst_framebuffer,
                 const ImageView& src_image_view);

    void ConvertDepthStencil(VkPipeline pipeline, const Framebuffer* dst_framebuffer,
                             ImageView& src_image_view);

    [[nodiscard]] VkPipeline FindOrEmplaceColorPipeline(const BlitImagePipelineKey& key);

    [[nodiscard]] VkPipeline FindOrEmplaceDepthStencilPipeline(const BlitImagePipelineKey& key);

    [[nodiscard]] VkPipeline FindOrEmplaceClearColorPipeline(const BlitImagePipelineKey& key);
    [[nodiscard]] VkPipeline FindOrEmplaceClearStencilPipeline(
        const BlitDepthStencilPipelineKey& key);
    [[nodiscard]] VkPipeline FindOrEmplaceMSAACopyPipeline(const MSAACopyPipelineKey& key);
    [[nodiscard]] VkPipeline FindOrEmplaceMSAACopyDepthPipeline(const MSAACopyPipelineKey& key,
                                                                bool copy_stencil);
    [[nodiscard]] VkPipeline FindOrEmplaceBlitColorMSAAPipeline(const BlitMSAAPipelineKey& key);
    [[nodiscard]] VkPipeline FindOrEmplaceBlitDepthStencilMSAAPipeline(
        const BlitMSAAPipelineKey& key, bool blit_stencil);

    void ConvertPipeline(vk::Pipeline& pipeline, VkRenderPass renderpass, bool is_target_depth);

    void ConvertDepthToColorPipeline(vk::Pipeline& pipeline, VkRenderPass renderpass);

    void ConvertColorToDepthPipeline(vk::Pipeline& pipeline, VkRenderPass renderpass);

    void ConvertPipelineEx(vk::Pipeline& pipeline, VkRenderPass renderpass,
                           vk::ShaderModule& module, bool single_texture, bool is_target_depth,
                           bool write_stencil = false);

    void ConvertPipelineColorTargetEx(vk::Pipeline& pipeline, VkRenderPass renderpass,
                                      vk::ShaderModule& module);

    void ConvertPipelineDepthTargetEx(vk::Pipeline& pipeline, VkRenderPass renderpass,
                                      vk::ShaderModule& module);

    void ConvertPipelineDepthStencilTargetEx(vk::Pipeline& pipeline, VkRenderPass renderpass,
                                             vk::ShaderModule& module);

    const Device& device;
    Scheduler& scheduler;
    StateTracker& state_tracker;

    vk::DescriptorSetLayout one_texture_set_layout;
    vk::DescriptorSetLayout two_textures_set_layout;
    DescriptorAllocator one_texture_descriptor_allocator;
    DescriptorAllocator two_textures_descriptor_allocator;
    vk::PipelineLayout one_texture_pipeline_layout;
    vk::PipelineLayout two_textures_pipeline_layout;
    vk::PipelineLayout clear_color_pipeline_layout;
    vk::PipelineLayout msaa_copy_pipeline_layout;
    vk::PipelineLayout msaa_copy_depth_stencil_pipeline_layout;
    vk::ShaderModule full_screen_vert;
    vk::ShaderModule blit_color_to_color_frag;
    vk::ShaderModule blit_color_msaa_frag;
    vk::ShaderModule blit_color_msaa_sint_frag;
    vk::ShaderModule blit_color_msaa_uint_frag;
    vk::ShaderModule blit_depth_msaa_frag;
    vk::ShaderModule blit_depth_stencil_msaa_frag;
    vk::ShaderModule blit_depth_stencil_frag;
    vk::ShaderModule clear_color_vert;
    vk::ShaderModule clear_color_frag;
    vk::ShaderModule clear_color_sint_frag;
    vk::ShaderModule clear_color_uint_frag;
    vk::ShaderModule clear_stencil_frag;
    vk::ShaderModule convert_depth_to_float_frag;
    vk::ShaderModule convert_float_to_depth_frag;
    vk::ShaderModule convert_abgr8_to_d24s8_frag;
    vk::ShaderModule convert_abgr8_to_s8d24_frag;
    vk::ShaderModule convert_abgr8_to_d32f_frag;
    vk::ShaderModule convert_d32f_to_abgr8_frag;
    vk::ShaderModule convert_d24s8_to_abgr8_frag;
    vk::ShaderModule convert_s8d24_to_abgr8_frag;
    vk::ShaderModule convert_non_msaa_to_msaa_frag;
    vk::ShaderModule convert_non_msaa_to_msaa_sint_frag;
    vk::ShaderModule convert_non_msaa_to_msaa_uint_frag;
    vk::ShaderModule convert_non_msaa_to_msaa_depth_frag;
    vk::ShaderModule convert_non_msaa_to_msaa_depth_stencil_frag;
    vk::ShaderModule convert_packed_depth_to_msaa_frag;
    vk::ShaderModule convert_packed_depth_stencil_to_msaa_frag;
    vk::Sampler linear_sampler;
    vk::Sampler nearest_sampler;

    std::vector<BlitImagePipelineKey> blit_color_keys;
    std::vector<vk::Pipeline> blit_color_pipelines;
    std::vector<BlitImagePipelineKey> blit_depth_stencil_keys;
    std::vector<vk::Pipeline> blit_depth_stencil_pipelines;
    std::vector<BlitImagePipelineKey> clear_color_keys;
    std::vector<vk::Pipeline> clear_color_pipelines;
    std::vector<BlitDepthStencilPipelineKey> clear_stencil_keys;
    std::vector<vk::Pipeline> clear_stencil_pipelines;
    std::vector<MSAACopyPipelineKey> msaa_copy_keys;
    std::vector<vk::Pipeline> msaa_copy_pipelines;
    size_t msaa_copy_last{};
    std::vector<MSAACopyPipelineKey> msaa_copy_depth_keys;
    std::vector<vk::Pipeline> msaa_copy_depth_pipelines;
    size_t msaa_copy_depth_last{};
    std::vector<MSAACopyPipelineKey> msaa_copy_depth_stencil_keys;
    std::vector<vk::Pipeline> msaa_copy_depth_stencil_pipelines;
    size_t msaa_copy_depth_stencil_last{};
    std::vector<BlitMSAAPipelineKey> blit_msaa_color_keys;
    std::vector<vk::Pipeline> blit_msaa_color_pipelines;
    size_t blit_msaa_color_last{};
    std::vector<BlitMSAAPipelineKey> blit_msaa_depth_keys;
    std::vector<vk::Pipeline> blit_msaa_depth_pipelines;
    size_t blit_msaa_depth_last{};
    std::vector<BlitMSAAPipelineKey> blit_msaa_depth_stencil_keys;
    std::vector<vk::Pipeline> blit_msaa_depth_stencil_pipelines;
    size_t blit_msaa_depth_stencil_last{};
    std::vector<std::pair<u64, vk::RenderPass>> native_resolve_passes;
    struct MSAACopyResources {
        u64 tick;
        vk::ImageView src_view;
        vk::ImageView dst_view;
        vk::Framebuffer framebuffer;
    };
    std::deque<MSAACopyResources> msaa_copy_resources;
    vk::Pipeline convert_d32_to_r32_pipeline;
    vk::Pipeline convert_r32_to_d32_pipeline;
    vk::Pipeline convert_d16_to_r16_pipeline;
    vk::Pipeline convert_r16_to_d16_pipeline;
    vk::Pipeline convert_abgr8_to_d24s8_pipeline;
    vk::Pipeline convert_abgr8_to_s8d24_pipeline;
    vk::Pipeline convert_abgr8_to_d32f_pipeline;
    vk::Pipeline convert_d32f_to_abgr8_pipeline;
    vk::Pipeline convert_d24s8_to_abgr8_pipeline;
    vk::Pipeline convert_s8d24_to_abgr8_pipeline;
};

} // namespace Vulkan
