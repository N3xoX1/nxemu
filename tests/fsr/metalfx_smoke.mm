// SPDX-License-Identifier: GPL-2.0-or-later
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    @autoreleasepool {
        if (@available(macOS 13.0, *)) {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device || ![MTLFXSpatialScalerDescriptor supportsDevice:device]) {
                std::cout << "MetalFX Spatial unsupported on this Mac\n";
                return 77;
            }
            MTLFXSpatialScalerDescriptor* desc = [[MTLFXSpatialScalerDescriptor alloc] init];
            desc.inputWidth = desc.inputHeight = 64;
            desc.outputWidth = desc.outputHeight = 128;
            desc.colorTextureFormat = desc.outputTextureFormat = MTLPixelFormatRGBA16Float;
            desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
            id<MTLFXSpatialScaler> scaler = [desc newSpatialScalerWithDevice:device];
            if (!scaler) { std::cerr << "RGBA16F MetalFX scaler creation failed\n"; return 1; }
            MTLTextureDescriptor* inputDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                MTLPixelFormatRGBA16Float width:64 height:64 mipmapped:NO];
            inputDesc.storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
            inputDesc.usage = scaler.colorTextureUsage;
            MTLTextureDescriptor* outputDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                MTLPixelFormatRGBA16Float width:128 height:128 mipmapped:NO];
            outputDesc.storageMode = MTLStorageModePrivate;
            outputDesc.usage = scaler.outputTextureUsage;
            id<MTLTexture> input = [device newTextureWithDescriptor:inputDesc];
            id<MTLTexture> output = [device newTextureWithDescriptor:outputDesc];
            id<MTLBuffer> readback = [device newBufferWithLength:128 * 128 * 8 options:MTLResourceStorageModeShared];
            id<MTLCommandQueue> queue = [device newCommandQueue];
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            if (!input || !output || !readback || !commands) return 1;
            std::vector<std::uint16_t> red(64 * 64 * 4, 0);
            for (size_t i = 0; i < red.size(); i += 4) red[i] = red[i + 3] = 0x3c00; // FP16 1.0
            [input replaceRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0 withBytes:red.data() bytesPerRow:64 * 8];
            scaler.colorTexture = input;
            scaler.outputTexture = output;
            scaler.inputContentWidth = scaler.inputContentHeight = 64;
            [scaler encodeToCommandBuffer:commands];
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            if (!blit) return 1;
            [blit copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(128, 128, 1) toBuffer:readback destinationOffset:0
                destinationBytesPerRow:128 * 8 destinationBytesPerImage:128 * 128 * 8];
            [blit endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            if (commands.status != MTLCommandBufferStatusCompleted) return 1;
            const auto* pixel = static_cast<const std::uint16_t*>(readback.contents) + (64 * 128 + 64) * 4;
            if (pixel[0] < 0x3800 || pixel[0] > 0x4000 || (pixel[1] & 0x7fff) > 0x2400 || (pixel[2] & 0x7fff) > 0x2400) {
                std::cerr << "MetalFX constant-color readback failed\n"; return 1;
            }
            std::cout << "MetalFX Spatial RGBA16F GPU encode and readback passed\n";
            return 0;
        }
        std::cout << "MetalFX Spatial requires macOS 13+\n";
        return 77;
    }
}
