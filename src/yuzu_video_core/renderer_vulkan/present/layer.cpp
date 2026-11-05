// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include "yuzu_video_core/area_sampling.h"
#include "yuzu_video_core/renderer_vulkan/present/scale_capture.h"

#include "yuzu_video_core/present.h"
#include "yuzu_video_core/present_sharpening.h"
#include "yuzu_video_core/fsr.h"
#include "yuzu_video_core/renderer_vulkan/vk_rasterizer.h"

#include "yuzu_common/settings.h"
#include "video_settings.h"
#include "yuzu_video_core/framebuffer_config.h"
#include "yuzu_video_core/renderer_vulkan/present/fsr.h"
#include "yuzu_video_core/renderer_vulkan/present/catmull_pass.h"
#include "yuzu_video_core/renderer_vulkan/present/fxaa.h"
#ifdef NXEMU_ENABLE_INTEL_CMAA2
#include "yuzu_video_core/renderer_vulkan/present/intel_cmaa2.h"
#endif
#include "yuzu_video_core/renderer_vulkan/present/layer.h"
#include "yuzu_video_core/renderer_vulkan/present/present_push_constants.h"
#include "yuzu_video_core/renderer_vulkan/present/smaa.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/vk_blit_screen.h"
#include "yuzu_video_core/textures/decoders.h"

namespace Vulkan {

namespace {

bool SupportsCatmullIntermediate(const Device& device) {
    constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (device.GetPhysical().GetFormatProperties(VK_FORMAT_R16G16B16A16_UNORM).optimalTilingFeatures & required) == required;
}

u32 GetBytesPerPixel(const Tegra::FramebufferConfig& framebuffer) {
    using namespace VideoCore::Surface;
    return BytesPerBlock(PixelFormatFromGPUPixelFormat(framebuffer.pixel_format));
}

std::size_t GetSizeInBytes(const Tegra::FramebufferConfig& framebuffer) {
    return static_cast<std::size_t>(framebuffer.stride) *
           static_cast<std::size_t>(framebuffer.height) * GetBytesPerPixel(framebuffer);
}

VkFormat GetFormat(const Tegra::FramebufferConfig& framebuffer) {
    switch (framebuffer.pixel_format) {
    case Service::android::PixelFormat::Rgba8888:
    case Service::android::PixelFormat::Rgbx8888:
        return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    case Service::android::PixelFormat::Rgb565:
        return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case Service::android::PixelFormat::Bgra8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
    default:
        UNIMPLEMENTED_MSG("Unknown framebuffer pixel format: {}",
                          static_cast<u32>(framebuffer.pixel_format));
        return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    }
}

} // Anonymous namespace

Layer::Layer(const Device& device_, MemoryAllocator& memory_allocator_, Scheduler& scheduler_,
             Tegra::MaxwellDeviceMemoryManager& device_memory_, size_t image_count_,
             VkExtent2D, VkDescriptorSetLayout layout, const PresentFilters& filters_)
    : device(device_), memory_allocator(memory_allocator_), scheduler(scheduler_),
      device_memory(device_memory_), filters(filters_), image_count(image_count_),
      max_image_dimension(device_.GetPhysical().GetProperties().limits.maxImageDimension2D),
      supports_catmull_intermediate(SupportsCatmullIntermediate(device_)) {
    CreateDescriptorPool();
    CreateDescriptorSets(layout);
}

Layer::~Layer() {
    ReleaseRawImages();
}

PresentScaleSelection Layer::ConfigureDraw(PresentPushConstants* out_push_constants,
                          VkDescriptorSet* out_descriptor_set, RasterizerVulkan& rasterizer,
                          VkSampler sampler, VkSampler bilinear_sampler, VkSampler area_sampler, ScalingFilter requested_filter, PresentSharpening sharpening, size_t image_index,
                          const Tegra::FramebufferConfig& framebuffer,
                          const Layout::FramebufferLayout& layout) {
    const auto texture_info = rasterizer.AccelerateDisplay(
        framebuffer, framebuffer.address + framebuffer.offset, framebuffer.stride);
    const u32 texture_width = texture_info ? texture_info->width : framebuffer.width;
    const u32 texture_height = texture_info ? texture_info->height : framebuffer.height;
    const u32 scaled_width = texture_info ? texture_info->scaled_width : texture_width;
    const u32 scaled_height = texture_info ? texture_info->scaled_height : texture_height;
    const bool use_accelerated = texture_info.has_value();

    RefreshResources(framebuffer);
    SetAntiAliasPass({scaled_width, scaled_height});

    // Finish any pending renderpass
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Wait(resource_ticks[image_index]);
    SCOPE_EXIT {
        resource_ticks[image_index] = scheduler.CurrentTick();
    };

    if (!use_accelerated) {
        UpdateRawImage(framebuffer, image_index);
    }

    VkImage source_image = texture_info ? texture_info->image : *raw_images[image_index];
    VkImageView source_image_view =
        texture_info ? texture_info->image_view : *raw_image_views[image_index];

    if (!filters.is_applet_capture && ScaleCapture::active) {
        ScaleCapture::Source(device,memory_allocator,scheduler,source_image_view,
            scaled_width,scaled_height,Tegra::NormalizeCrop(framebuffer,texture_width,texture_height),
            layout,static_cast<unsigned>(VideoCore::fsr_frame_state.downscale_filter),
            static_cast<unsigned>(filters.get_anti_aliasing()));
    }
    anti_alias->Draw(scheduler, image_index, &source_image, &source_image_view);

    auto crop_rect = Tegra::NormalizeCrop(framebuffer, texture_width, texture_height);
    VideoCore::NotePresentInput(scaled_width, scaled_height,
        static_cast<unsigned>(std::lround(std::abs(crop_rect.right - crop_rect.left) * static_cast<float>(scaled_width))),
        static_cast<unsigned>(std::lround(std::abs(crop_rect.bottom - crop_rect.top) * static_cast<float>(scaled_height))));
    const VkExtent2D render_extent{
        .width = scaled_width,
        .height = scaled_height,
    };

    const auto selection = ResolvePresentScale(requested_filter,
        !filters.is_applet_capture && VideoCore::fsr_frame_state.downscale_filter == 1,
        scaled_width, scaled_height, crop_rect, layout.screen.GetWidth(), layout.screen.GetHeight());
    const ScalingFilter scaling_filter = selection.filter;
    VideoCore::NotePresentSelection(static_cast<unsigned>(scaling_filter),
                                   static_cast<unsigned>(selection.mode));
    if (selection.mode != PresentScaleMode::Upscale) {
        sampler = scaling_filter == ScalingFilter::Area ? area_sampler : bilinear_sampler;
    }
    if (scaling_filter == ScalingFilter::Area) {
        const std::array<float, 4> area{
            std::abs(crop_rect.right - crop_rect.left) * scaled_width / layout.screen.GetWidth(),
            std::abs(crop_rect.bottom - crop_rect.top) * scaled_height / layout.screen.GetHeight(),
            1.0f / scaled_width, 1.0f / scaled_height};
        for (size_t i = 0; i < area.size(); ++i)
            out_push_constants->filter_constants[i] = std::bit_cast<u32>(area[i]);
        out_push_constants->filter_constants[4] = VideoCore::AreaSamplingGrid(scaled_width, scaled_height,
            crop_rect.left, crop_rect.top, crop_rect.right, crop_rect.bottom,
            layout.screen.GetWidth(), layout.screen.GetHeight());
    }
    // Sharpen the reduced image, never the original high-resolution texels.
    // Bilinear+CAS already reconstructs its output-pixel cross in one pass.
    // Area is evaluated once per output pixel; RCAS needs texelFetch on an
    // actual output-sized image. Keep both cached passes alive across toggles.
    const bool rcas_phase_copy = selection.mode == PresentScaleMode::Native &&
        sharpening == PresentSharpening::Rcas && layout.screen.GetWidth() &&
        layout.screen.GetHeight() && !IsPresentCropTexelAligned(scaled_width, scaled_height, crop_rect);
    const bool precompute_downscale = rcas_phase_copy || (selection.mode == PresentScaleMode::Downscale &&
        sharpening != PresentSharpening::None &&
        (scaling_filter == ScalingFilter::Area || sharpening == PresentSharpening::Rcas));
    if (precompute_downscale) {
        auto& pass = downscale_passes[scaling_filter == ScalingFilter::Area ? 1 : 0];
        if (!pass) {
            pass = std::make_unique<CatmullPass>(device, memory_allocator, image_count,
                VkExtent2D{layout.screen.GetWidth(), layout.screen.GetHeight()}, scaling_filter);
        }
        source_image_view = pass->Draw(scheduler, image_index, source_image, source_image_view,
                                      crop_rect, false, out_push_constants);
        crop_rect = pass->OutputCrop();
        sampler = bilinear_sampler;
        out_push_constants->filter_constants.fill(0);
        VideoCore::NoteFsrInput(scaled_width, scaled_height, 1U);
    }
    const bool catmull_cas = scaling_filter == ScalingFilter::CasCatmullRom ||
                             scaling_filter == ScalingFilter::CasCatmullRomFast;
    // Avoid a second pass for native mapping and small targets where its setup costs more.
    // High CAS strength amplifies tiny reconstruction-rounding differences; keep it fused.
    const bool precompute_catmull = catmull_cas && supports_catmull_intermediate &&
        videoSettings.cas_sharpening_slider <= 66 &&
        u64(layout.screen.GetWidth()) * layout.screen.GetHeight() >= 1024U * 1024U &&
        layout.screen.GetWidth() <= max_image_dimension - 2 &&
        layout.screen.GetHeight() <= max_image_dimension - 2 &&
        !CanUseNativeRcas(scaled_width, scaled_height, crop_rect,
                         layout.screen.GetWidth(), layout.screen.GetHeight());
    if (precompute_catmull) {
        if (!catmull_pass) {
            catmull_pass = std::make_unique<CatmullPass>(device, memory_allocator, image_count,
                VkExtent2D{layout.screen.GetWidth(), layout.screen.GetHeight()});
        }
        source_image_view = catmull_pass->Draw(scheduler, image_index, source_image,
            source_image_view, crop_rect, scaling_filter == ScalingFilter::CasCatmullRomFast);
        crop_rect = catmull_pass->OutputCrop();
        VideoCore::NoteFsrInput(scaled_width, scaled_height, 1U);
    }
    const bool use_fsr = scaling_filter == ScalingFilter::Fsr ||
                         scaling_filter == ScalingFilter::FsrEasu;
    const bool skip_easu = !VideoCore::LegacyFsr() && use_fsr && CanUseNativeRcas(
        scaled_width, scaled_height, crop_rect, layout.screen.GetWidth(),
        layout.screen.GetHeight());
    if (use_fsr) VideoCore::NoteFsrInput(scaled_width, scaled_height,
        VideoCore::LegacyFsr() ? 2U : (scaling_filter == ScalingFilter::Fsr && !skip_easu ? 1U : 0U));
    if (scaling_filter == ScalingFilter::Fsr && !skip_easu) {
        if (!fsr) {
            CreateFSR({layout.screen.GetWidth(), layout.screen.GetHeight()}, !VideoCore::LegacyFsr());
        }
        source_image_view = fsr->Draw(scheduler, image_index, source_image, source_image_view,
                                      render_extent, crop_rect);
        crop_rect = {0, 0, 1, 1};
    }

    SetMatrixData(*out_push_constants, layout);
    SetVertexData(*out_push_constants, layout, crop_rect);
    if (scaling_filter == ScalingFilter::CatmullRom || scaling_filter == ScalingFilter::CasCatmullRom ||
        scaling_filter == ScalingFilter::CatmullRomFast || scaling_filter == ScalingFilter::CasCatmullRomFast) {
        out_push_constants->filter_constants[1] = CanUseNativeRcas(scaled_width, scaled_height,
            crop_rect, layout.screen.GetWidth(), layout.screen.GetHeight()) ? 1U : 0U;
        if (scaling_filter == ScalingFilter::CatmullRomFast || scaling_filter == ScalingFilter::CasCatmullRomFast)
            out_push_constants->filter_constants[1] |= 2U;
    }

    if (precompute_catmull) out_push_constants->filter_constants[1] |= 4U;

    if (scaling_filter == ScalingFilter::FsrEasu && !skip_easu) {
        std::array<u32, 16> easu_constants{};
        const float input_width = static_cast<float>(scaled_width);
        const float input_height = static_cast<float>(scaled_height);
        const float viewport_width = (crop_rect.right - crop_rect.left) * input_width;
        const float viewport_height = (crop_rect.bottom - crop_rect.top) * input_height;
        ::FSR::FsrEasuConOffset(easu_constants.data(), easu_constants.data() + 4,
                                easu_constants.data() + 8, easu_constants.data() + 12,
                                viewport_width, viewport_height, input_width, input_height,
                                static_cast<float>(layout.screen.GetWidth()),
                                static_cast<float>(layout.screen.GetHeight()),
                                crop_rect.left * input_width, crop_rect.top * input_height);
        std::copy_n(easu_constants.begin(), 12, out_push_constants->filter_constants.begin());
    }

    UpdateDescriptorSet(source_image_view, sampler, image_index);
    *out_descriptor_set = descriptor_sets[image_index];
    return selection;
}

void Layer::CreateDescriptorPool() {
    descriptor_pool = CreateWrappedDescriptorPool(device, image_count, image_count);
}

void Layer::CreateDescriptorSets(VkDescriptorSetLayout layout) {
    const std::vector layouts(image_count, layout);
    descriptor_sets = CreateWrappedDescriptorSets(descriptor_pool, layouts);
}

void Layer::CreateStagingBuffer(const Tegra::FramebufferConfig& framebuffer) {
    const VkBufferCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = CalculateBufferSize(framebuffer),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };

    buffer = memory_allocator.CreateBuffer(ci, MemoryUsage::Upload);
}

void Layer::CreateRawImages(const Tegra::FramebufferConfig& framebuffer) {
    const auto format = GetFormat(framebuffer);
    resource_ticks.resize(image_count);
    raw_images.resize(image_count);
    raw_image_views.resize(image_count);

    for (size_t i = 0; i < image_count; ++i) {
        raw_images[i] =
            CreateWrappedImage(memory_allocator, {framebuffer.width, framebuffer.height}, format);
        raw_image_views[i] = CreateWrappedImageView(device, raw_images[i], format);
    }
}

void Layer::CreateFSR(VkExtent2D output_size, bool easu_only) {
    fsr = std::make_unique<FSR>(device, memory_allocator, image_count, output_size, easu_only);
}

void Layer::RefreshResources(const Tegra::FramebufferConfig& framebuffer) {
    if (framebuffer.width == raw_width && framebuffer.height == raw_height &&
        framebuffer.pixel_format == pixel_format && !raw_images.empty()) {
        return;
    }

    for (const u64 tick : resource_ticks) {
        scheduler.Wait(tick);
    }

    raw_width = framebuffer.width;
    raw_height = framebuffer.height;
    pixel_format = framebuffer.pixel_format;
    ReleaseRawImages();
    CreateStagingBuffer(framebuffer);
    CreateRawImages(framebuffer);
}

void Layer::SetAntiAliasPass(VkExtent2D source_extent) {
    const auto setting = filters.get_anti_aliasing();
    if (anti_alias && anti_alias_setting == setting &&
        (setting == AntiAliasing::None ||
         (anti_alias_extent.width == source_extent.width &&
          anti_alias_extent.height == source_extent.height))) {
        return;
    }

    anti_alias_setting = setting;
    anti_alias_extent = source_extent;

    // All slots may still reference the old pass, including recorded commands.
    for (const u64 tick : resource_ticks) {
        scheduler.Wait(tick);
    }

    switch (anti_alias_setting) {
#ifdef NXEMU_ENABLE_INTEL_CMAA2
    case AntiAliasing::Cmaa2Intel:
        try {
            anti_alias = std::make_unique<IntelCMAA2>(device, memory_allocator, image_count, source_extent);
        } catch (const std::exception& e) {
            LOG_ERROR(Render_Vulkan, "CMAA2 unavailable: {}. Falling back to FXAA.", e.what());
            anti_alias = std::make_unique<FXAA>(device, memory_allocator, image_count, source_extent);
        }
        break;
#endif
    case AntiAliasing::Fxaa:
        anti_alias = std::make_unique<FXAA>(device, memory_allocator, image_count, source_extent);
        break;
    case AntiAliasing::Smaa:
    case AntiAliasing::SmaaHigh:
        anti_alias = std::make_unique<SMAA>(device, memory_allocator, image_count, source_extent,
                                          anti_alias_setting == AntiAliasing::Smaa);
        break;
    default:
        anti_alias = std::make_unique<NoAA>();
        break;
    }
}

void Layer::ReleaseRawImages() {
    for (const u64 tick : resource_ticks) {
        scheduler.Wait(tick);
    }
    raw_images.clear();
    buffer.reset();
}

u64 Layer::CalculateBufferSize(const Tegra::FramebufferConfig& framebuffer) const {
    return GetSizeInBytes(framebuffer) * image_count;
}

u64 Layer::GetRawImageOffset(const Tegra::FramebufferConfig& framebuffer,
                             size_t image_index) const {
    return GetSizeInBytes(framebuffer) * image_index;
}

void Layer::SetMatrixData(PresentPushConstants& data,
                          const Layout::FramebufferLayout& layout) const {
    data.transform =
        MakeOrthographicTransform(static_cast<f32>(layout.width), static_cast<f32>(layout.height));
}

void Layer::SetVertexData(PresentPushConstants& data, const Layout::FramebufferLayout& layout,
                          const Common::Rectangle<f32>& crop) const {
    // Map the coordinates to the screen.
    const auto& screen = layout.screen;
    const auto x = static_cast<f32>(screen.left);
    const auto y = static_cast<f32>(screen.top);
    const auto w = static_cast<f32>(screen.GetWidth());
    const auto h = static_cast<f32>(screen.GetHeight());

    data.vertices[0] = ScreenRectVertex(x, y, crop.left, crop.top);
    data.vertices[1] = ScreenRectVertex(x + w, y, crop.right, crop.top);
    data.vertices[2] = ScreenRectVertex(x, y + h, crop.left, crop.bottom);
    data.vertices[3] = ScreenRectVertex(x + w, y + h, crop.right, crop.bottom);
}

void Layer::UpdateDescriptorSet(VkImageView image_view, VkSampler sampler, size_t image_index) {
    const VkDescriptorImageInfo image_info{
        .sampler = sampler,
        .imageView = image_view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };

    const VkWriteDescriptorSet sampler_write{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_sets[image_index],
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };

    device.GetLogical().UpdateDescriptorSets(std::array{sampler_write}, {});
}

void Layer::UpdateRawImage(const Tegra::FramebufferConfig& framebuffer, size_t image_index) {
    const std::span<u8> mapped_span = buffer.Mapped();

    const u64 image_offset = GetRawImageOffset(framebuffer, image_index);

    const DAddr framebuffer_addr = framebuffer.address + framebuffer.offset;
    const u8* const host_ptr = device_memory.GetPointer<u8>(framebuffer_addr);

    // TODO(Rodrigo): Read this from HLE
    constexpr u32 block_height_log2 = 4;
    const u32 bytes_per_pixel = GetBytesPerPixel(framebuffer);
    const u64 linear_size{GetSizeInBytes(framebuffer)};
    const u64 tiled_size{Tegra::Texture::CalculateSize(
        true, bytes_per_pixel, framebuffer.stride, framebuffer.height, 1, block_height_log2, 0)};
    if (host_ptr) {
        Tegra::Texture::UnswizzleTexture(
            mapped_span.subspan(image_offset, linear_size), std::span(host_ptr, tiled_size),
            bytes_per_pixel, framebuffer.width, framebuffer.height, 1, block_height_log2, 0);
    }

    const VkBufferImageCopy copy{
        .bufferOffset = image_offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {.x = 0, .y = 0, .z = 0},
        .imageExtent =
            {
                .width = framebuffer.width,
                .height = framebuffer.height,
                .depth = 1,
            },
    };
    scheduler.Record([this, copy, index = image_index](vk::CommandBuffer cmdbuf) {
        const VkImage image = *raw_images[index];
        const VkImageMemoryBarrier base_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        VkImageMemoryBarrier read_barrier = base_barrier;
        read_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        read_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        read_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        VkImageMemoryBarrier write_barrier = base_barrier;
        write_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        write_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        write_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               read_barrier);
        cmdbuf.CopyBufferToImage(*buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, write_barrier);
    });
}

} // namespace Vulkan
