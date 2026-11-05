// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/renderer_vulkan/present/upscaler.h"
#include "yuzu_video_core/renderer_vulkan/present/catmull_pass.h"
#include "yuzu_video_core/renderer_vulkan/present/util.h"
#include "yuzu_video_core/renderer_vulkan/vk_scheduler.h"
#include "yuzu_video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
using namespace VideoCore::Upscaling;
class API_AVAILABLE(macos(13.0)) MetalFxSpatial final : public Upscaler {
public:
    MetalFxSpatial(const Device& device_, MemoryAllocator& allocator_, size_t count_, VkExtent2D output_)
        : device(device_), allocator(allocator_), count(count_), output(output_) {
        if (!device.IsMetalObjectsEnabled()) throw std::runtime_error("VK_EXT_metal_objects is unavailable");
        export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(device.GetDispatchLoader().vkGetDeviceProcAddr(
            *device.GetLogical(), "vkExportMetalObjectsEXT"));
        if (!export_objects) throw std::runtime_error("vkExportMetalObjectsEXT is unavailable");
        VkExportMetalCommandQueueInfoEXT queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT;
        queue_info.queue = *device.GetGraphicsQueue();
        VkExportMetalDeviceInfoEXT device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT;
        device_info.pNext = &queue_info;
        VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &device_info};
        export_objects(*device.GetLogical(), &info);
        native_device = device_info.mtlDevice;
        queue = queue_info.mtlCommandQueue;
        if (!native_device || !queue || ![MTLFXSpatialScalerDescriptor supportsDevice:native_device])
            throw std::runtime_error("MetalFX Spatial is unsupported on the selected Vulkan GPU");
        if (const char* path = std::getenv("NXEMU_METALFX_CSV")) {
            timings.open(path, std::ios::app);
            if (timings && timings.tellp() == std::streampos(0))
                timings << "input_width,input_height,output_width,output_height,synchronized_cpu_ms,metalfx_gpu_ms\n";
        }
        LOG_INFO(Render_Vulkan, "MetalFX Spatial prototype selected; CPU-synchronized interop, visual comparison only");
    }
    VideoCore::Upscaling::Backend Backend() const override { return VideoCore::Upscaling::Backend::MetalFxSpatial; }
    VideoCore::Upscaling::Capabilities Capabilities() const override { return {.cpu_synchronized = true}; }
    std::optional<UpscaleResult> Draw(Scheduler& scheduler, const UpscaleFrame& frame) override {
        if (disabled || !IsFrameSupported(Capabilities(), frame) || frame.image_index >= count ||
            frame.parameters.output.width != output.width || frame.parameters.output.height != output.height) return std::nullopt;
        @autoreleasepool {
            @try {
                try { return DrawFrame(scheduler, frame); }
                catch (const std::exception& e) {
                    disabled = true;
                    LOG_WARNING(Render_Vulkan, "MetalFX Spatial disabled, reverting to FSR: {}", e.what());
                }
            } @catch (NSException* e) {
                disabled = true;
                LOG_WARNING(Render_Vulkan, "MetalFX Spatial exception, reverting to FSR: {}", [[e reason] UTF8String]);
            }
        }
        return std::nullopt;
    }
private:
    std::optional<UpscaleResult> DrawFrame(Scheduler& scheduler, const UpscaleFrame& frame) {
        const auto start = std::chrono::steady_clock::now();
        const VkExtent2D size{frame.parameters.input.width, frame.parameters.input.height};
        if (!normalize || input.width != size.width || input.height != size.height || encoding != frame.parameters.encoding) {
            // Previous slots can still be sampled by Vulkan; wait before replacing the cache.
            scheduler.Finish();
            input = size;
            encoding = frame.parameters.encoding;
            const VkExportMetalObjectCreateInfoEXT export_image{
                VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT, nullptr,
                VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT};
            normalize = std::make_unique<CatmullPass>(device, allocator, count, input,
                ScalingFilter::Bilinear, VK_FORMAT_R16G16B16A16_SFLOAT, &export_image);
            slots.clear();
            slots.resize(count);
            for (auto& slot : slots) {
                slot.image = CreateWrappedImage(allocator, output, VK_FORMAT_R16G16B16A16_SFLOAT, &export_image);
                slot.view = CreateWrappedImageView(device, slot.image, VK_FORMAT_R16G16B16A16_SFLOAT);
            }
            MTLFXSpatialScalerDescriptor* descriptor = [[MTLFXSpatialScalerDescriptor alloc] init];
            descriptor.inputWidth = input.width;
            descriptor.inputHeight = input.height;
            descriptor.outputWidth = output.width;
            descriptor.outputHeight = output.height;
            descriptor.colorTextureFormat = MTLPixelFormatRGBA16Float;
            descriptor.outputTextureFormat = MTLPixelFormatRGBA16Float;
            descriptor.colorProcessingMode = encoding == ColorEncoding::Linear
                ? MTLFXSpatialScalerColorProcessingModeLinear : MTLFXSpatialScalerColorProcessingModePerceptual;
            scaler = [descriptor newSpatialScalerWithDevice:native_device];
            if (!scaler) throw std::runtime_error("MetalFX rejected the SDR RGBA16F scaler configuration");
            fence = [native_device newFence];
            if (!fence) throw std::runtime_error("Metal fence creation failed");
            scaler.fence = fence;
        }
        // Normalize only the useful crop, including flips, to the exact MetalFX input extent.
        normalize->Draw(scheduler, frame.image_index, frame.color, frame.color_view, frame.crop, false);
        auto& slot = slots.at(frame.image_index);
        const VkImage destination = *slot.image;
        const bool initialized = slot.initialized;
        slot.initialized = true;
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([destination, initialized](vk::CommandBuffer cb) {
            TransitionImageLayout(cb, destination, VK_IMAGE_LAYOUT_GENERAL,
                initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED);
        });
        // Intentional prototype boundary: no native work races Vulkan or pending worker commands.
        // Replace with exported GPU events before drawing production-performance conclusions.
        scheduler.Finish();
        VkExportMetalTextureInfoEXT dst{};
        dst.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT;
        dst.image = destination;
        dst.plane = VK_IMAGE_ASPECT_COLOR_BIT;
        VkExportMetalTextureInfoEXT src{};
        src.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT;
        src.pNext = &dst;
        src.image = normalize->ImageAt(frame.image_index);
        src.plane = VK_IMAGE_ASPECT_COLOR_BIT;
        VkExportMetalObjectsInfoEXT info{VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &src};
        export_objects(*device.GetLogical(), &info);
        id<MTLTexture> color = src.mtlTexture;
        id<MTLTexture> result = dst.mtlTexture;
        if (!color || !result) throw std::runtime_error("MoltenVK did not export the scaler textures");
        if ((color.usage & scaler.colorTextureUsage) != scaler.colorTextureUsage ||
            (result.usage & scaler.outputTextureUsage) != scaler.outputTextureUsage ||
            color.pixelFormat != MTLPixelFormatRGBA16Float || result.pixelFormat != MTLPixelFormatRGBA16Float)
            throw std::runtime_error("Exported Metal textures do not meet MetalFX format/usage requirements");
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        if (!commands) throw std::runtime_error("Metal command buffer creation failed");
        commands.label = @"NXEmu MetalFX Spatial comparison";
        scaler.colorTexture = color;
        scaler.outputTexture = result;
        scaler.inputContentWidth = input.width;
        scaler.inputContentHeight = input.height;
        // MoltenVK textures may be untracked. Seed the fence consumed by MetalFX's internal passes.
        id<MTLBlitCommandEncoder> before = [commands blitCommandEncoder];
        if (!before) throw std::runtime_error("Metal fence encoder creation failed");
        [before updateFence:fence];
        [before endEncoding];
        [scaler encodeToCommandBuffer:commands];
        {
            // MoltenVK and the native adapter submit to the same underlying queue.
            std::scoped_lock lock{device.QueueMutex()};
            [commands commit];
        }
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted)
            throw std::runtime_error("MetalFX command buffer failed");
        scheduler.Record([destination](vk::CommandBuffer cb) {
            TransitionImageLayout(cb, destination, VK_IMAGE_LAYOUT_GENERAL);
        });
        if (timings) {
            const double cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            timings << input.width << ',' << input.height << ',' << output.width << ',' << output.height << ','
                    << cpu_ms << ',' << (commands.GPUEndTime - commands.GPUStartTime) * 1000.0 << '\n';
        }
        return UpscaleResult{*slot.view};
    }
    const Device& device;
    MemoryAllocator& allocator;
    const size_t count;
    const VkExtent2D output;
    VkExtent2D input{};
    ColorEncoding encoding{ColorEncoding::DisplayReferred};
    PFN_vkExportMetalObjectsEXT export_objects{};
    id<MTLDevice> native_device;
    id<MTLCommandQueue> queue;
    id<MTLFXSpatialScaler> scaler;
    id<MTLFence> fence;
    std::unique_ptr<CatmullPass> normalize;
    struct Slot { vk::Image image; vk::ImageView view; bool initialized{}; };
    std::vector<Slot> slots;
    std::ofstream timings;
    bool disabled{};
};
} // namespace
std::unique_ptr<Upscaler> CreateMetalFxSpatial(const Device& device, MemoryAllocator& allocator,
                                             size_t count, VkExtent2D output) {
    @autoreleasepool {
        @try {
        if (@available(macOS 13.0, *)) {
            try { return std::make_unique<MetalFxSpatial>(device, allocator, count, output); }
            catch (const std::exception& e) {
                LOG_WARNING(Render_Vulkan, "MetalFX Spatial unavailable, keeping FSR: {}", e.what());
            }
        } else {
            LOG_WARNING(Render_Vulkan, "MetalFX Spatial requires macOS 13 or later; keeping FSR");
        }
        } @catch (NSException* e) {
            LOG_WARNING(Render_Vulkan, "MetalFX initialization exception, keeping FSR: {}", [[e reason] UTF8String]);
        }
    }
    return {};
}
} // namespace Vulkan
