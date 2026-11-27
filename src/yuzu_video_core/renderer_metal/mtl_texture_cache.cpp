// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <string>

#include <boost/container/small_vector.hpp>

#include "yuzu_common/alignment.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/virtual_buffer.h"
#include "video_settings.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/maxwell_to_mtl.h"
#include "yuzu_video_core/renderer_metal/mtl_scheduler.h"
#include "yuzu_video_core/renderer_metal/mtl_texture_cache.h"
#include "yuzu_video_core/texture_cache/decode_bc.h"
#include "yuzu_video_core/texture_cache/expand_format.h"
#include "yuzu_video_core/texture_cache/formatter.h"
#include "yuzu_video_core/texture_cache/samples_helper.h"
#include "yuzu_video_core/texture_cache/util.h"

namespace Metal {

namespace {

// Round a copy extent up to the format's compressed block size. Metal addresses
// compressed textures in pixels but copies whole blocks, so a sub-block extent
// is illegal, and the last mips of a block-compressed chain always are. Without
// the validation layer Metal just takes the copy and gives an undefined result.
// It doesn't overread, a mip's storage is always whole blocks.
[[nodiscard]] MTL::Size AlignCopySizeToBlock(MTL::Size size, MTL::PixelFormat format) {
    u32 block_width = 1;
    u32 block_height = 1;
    switch (format) {
    case MTL::PixelFormatBC1_RGBA:
    case MTL::PixelFormatBC1_RGBA_sRGB:
    case MTL::PixelFormatBC2_RGBA:
    case MTL::PixelFormatBC2_RGBA_sRGB:
    case MTL::PixelFormatBC3_RGBA:
    case MTL::PixelFormatBC3_RGBA_sRGB:
    case MTL::PixelFormatBC4_RUnorm:
    case MTL::PixelFormatBC4_RSnorm:
    case MTL::PixelFormatBC5_RGUnorm:
    case MTL::PixelFormatBC5_RGSnorm:
    case MTL::PixelFormatBC6H_RGBFloat:
    case MTL::PixelFormatBC6H_RGBUfloat:
    case MTL::PixelFormatBC7_RGBAUnorm:
    case MTL::PixelFormatBC7_RGBAUnorm_sRGB:
    case MTL::PixelFormatEAC_R11Unorm:
    case MTL::PixelFormatEAC_R11Snorm:
    case MTL::PixelFormatEAC_RG11Unorm:
    case MTL::PixelFormatEAC_RG11Snorm:
    case MTL::PixelFormatEAC_RGBA8:
    case MTL::PixelFormatEAC_RGBA8_sRGB:
    case MTL::PixelFormatETC2_RGB8:
    case MTL::PixelFormatETC2_RGB8_sRGB:
    case MTL::PixelFormatETC2_RGB8A1:
    case MTL::PixelFormatETC2_RGB8A1_sRGB:
    case MTL::PixelFormatASTC_4x4_LDR:
    case MTL::PixelFormatASTC_4x4_sRGB:
    case MTL::PixelFormatASTC_4x4_HDR:
        block_width = 4;
        block_height = 4;
        break;
    case MTL::PixelFormatASTC_5x4_LDR:
    case MTL::PixelFormatASTC_5x4_sRGB:
    case MTL::PixelFormatASTC_5x4_HDR:
        block_width = 5;
        block_height = 4;
        break;
    case MTL::PixelFormatASTC_5x5_LDR:
    case MTL::PixelFormatASTC_5x5_sRGB:
    case MTL::PixelFormatASTC_5x5_HDR:
        block_width = 5;
        block_height = 5;
        break;
    case MTL::PixelFormatASTC_6x5_LDR:
    case MTL::PixelFormatASTC_6x5_sRGB:
    case MTL::PixelFormatASTC_6x5_HDR:
        block_width = 6;
        block_height = 5;
        break;
    case MTL::PixelFormatASTC_6x6_LDR:
    case MTL::PixelFormatASTC_6x6_sRGB:
    case MTL::PixelFormatASTC_6x6_HDR:
        block_width = 6;
        block_height = 6;
        break;
    case MTL::PixelFormatASTC_8x5_LDR:
    case MTL::PixelFormatASTC_8x5_sRGB:
    case MTL::PixelFormatASTC_8x5_HDR:
        block_width = 8;
        block_height = 5;
        break;
    case MTL::PixelFormatASTC_8x6_LDR:
    case MTL::PixelFormatASTC_8x6_sRGB:
    case MTL::PixelFormatASTC_8x6_HDR:
        block_width = 8;
        block_height = 6;
        break;
    case MTL::PixelFormatASTC_8x8_LDR:
    case MTL::PixelFormatASTC_8x8_sRGB:
    case MTL::PixelFormatASTC_8x8_HDR:
        block_width = 8;
        block_height = 8;
        break;
    case MTL::PixelFormatASTC_10x5_LDR:
    case MTL::PixelFormatASTC_10x5_sRGB:
    case MTL::PixelFormatASTC_10x5_HDR:
        block_width = 10;
        block_height = 5;
        break;
    case MTL::PixelFormatASTC_10x6_LDR:
    case MTL::PixelFormatASTC_10x6_sRGB:
    case MTL::PixelFormatASTC_10x6_HDR:
        block_width = 10;
        block_height = 6;
        break;
    case MTL::PixelFormatASTC_10x8_LDR:
    case MTL::PixelFormatASTC_10x8_sRGB:
    case MTL::PixelFormatASTC_10x8_HDR:
        block_width = 10;
        block_height = 8;
        break;
    case MTL::PixelFormatASTC_10x10_LDR:
    case MTL::PixelFormatASTC_10x10_sRGB:
    case MTL::PixelFormatASTC_10x10_HDR:
        block_width = 10;
        block_height = 10;
        break;
    case MTL::PixelFormatASTC_12x10_LDR:
    case MTL::PixelFormatASTC_12x10_sRGB:
    case MTL::PixelFormatASTC_12x10_HDR:
        block_width = 12;
        block_height = 10;
        break;
    case MTL::PixelFormatASTC_12x12_LDR:
    case MTL::PixelFormatASTC_12x12_sRGB:
    case MTL::PixelFormatASTC_12x12_HDR:
        block_width = 12;
        block_height = 12;
        break;
    default:
        return size;
    }

    return MTL::Size::Make(Common::AlignUp<NS::UInteger>(size.width, block_width),
                           Common::AlignUp<NS::UInteger>(size.height, block_height), size.depth);
}

// Whether Metal's validation layer is running. It aborts on things we do on
// purpose, so a couple of places relax what they do to keep the tool usable.
// Each one logs when it does.
[[nodiscard]] bool ValidationLayerActive() {
    static const bool active = [] {
        const char* const debug_layer = std::getenv("MTL_DEBUG_LAYER");
        const char* const shader_validation = std::getenv("MTL_SHADER_VALIDATION");
        const auto set = [](const char* value) { return value != nullptr && value[0] == '1'; };
        return set(debug_layer) || set(shader_validation);
    }();
    return active;
}

} // Anonymous namespace


using VideoCommon::ImageInfo;
using VideoCommon::SubresourceRange;
using VideoCore::Surface::IsPixelFormatASTC;
using VideoCore::Surface::SurfaceType;

namespace {

MTL::TextureType ToTextureType(const ImageInfo& info) {
    switch (info.type) {
    // Allocated as 2D with height 1 to match texture_1D_as_2D in the shader
    // translation. A texture1d can't carry mips, and a 1D view over a 2D base
    // isn't a legal Metal view class.
    case VideoCommon::ImageType::e1D:
        return info.resources.layers > 1 ? MTL::TextureType2DArray : MTL::TextureType2D;
    case VideoCommon::ImageType::e2D:
        if (info.num_samples > 1) {
            return info.resources.layers > 1 ? MTL::TextureType2DMultisampleArray
                                             : MTL::TextureType2DMultisample;
        }
        return info.resources.layers > 1 ? MTL::TextureType2DArray : MTL::TextureType2D;
    case VideoCommon::ImageType::e3D:
        return MTL::TextureType3D;
    case VideoCommon::ImageType::Linear:
        return MTL::TextureType2D;
    case VideoCommon::ImageType::Buffer:
        return MTL::TextureTypeTextureBuffer;
    }
    return MTL::TextureType2D;
}

MTL::TextureType ToTextureType(Shader::TextureType type) {
    switch (type) {
    // 1D is declared as 2D in the generated MSL (texture_1D_as_2D), MSL's
    // texture1d has no mipmaps and no LOD selection. The shader's declared type
    // and the view we hand it have to agree.
    case Shader::TextureType::Color1D:
        return MTL::TextureType2D;
    case Shader::TextureType::ColorArray1D:
        return MTL::TextureType2DArray;
    case Shader::TextureType::Color2D:
    case Shader::TextureType::Color2DRect:
        return MTL::TextureType2D;
    case Shader::TextureType::ColorArray2D:
        return MTL::TextureType2DArray;
    case Shader::TextureType::Color3D:
        return MTL::TextureType3D;
    case Shader::TextureType::ColorCube:
        return MTL::TextureTypeCube;
    case Shader::TextureType::ColorArrayCube:
        return MTL::TextureTypeCubeArray;
    case Shader::TextureType::Buffer:
        return MTL::TextureTypeTextureBuffer;
    }
    return MTL::TextureType2D;
}

MTL::TextureSwizzle ToSwizzle(Tegra::Texture::SwizzleSource source) {
    using Tegra::Texture::SwizzleSource;
    switch (source) {
    case SwizzleSource::Zero:
        return MTL::TextureSwizzleZero;
    case SwizzleSource::R:
        return MTL::TextureSwizzleRed;
    case SwizzleSource::G:
        return MTL::TextureSwizzleGreen;
    case SwizzleSource::B:
        return MTL::TextureSwizzleBlue;
    case SwizzleSource::A:
        return MTL::TextureSwizzleAlpha;
    case SwizzleSource::OneInt:
    case SwizzleSource::OneFloat:
        return MTL::TextureSwizzleOne;
    }
    return MTL::TextureSwizzleZero;
}

// Component mapping for a shader-visible view. That's the guest's own swizzle
// from the texture header plus the undo for whatever channels the format
// substitution moved around.
MTL::TextureSwizzleChannels MakeSwizzle(const VideoCommon::ImageViewInfo& info,
                                        SwizzleFix fix) {
    using Tegra::Texture::SwizzleSource;
    std::array<SwizzleSource, 4> sources{SwizzleSource::R, SwizzleSource::G, SwizzleSource::B,
                                         SwizzleSource::A};
    // A render target's view has no guest swizzle to honour. ImageViewInfo puts
    // the sentinel 0xff in all four channels, which isn't a SwizzleSource and
    // would map to Zero, so the target would sample as (0,0,0,0).
    if (!info.IsRenderTarget()) {
        sources = info.Swizzle();

        // A depth/stencil image has one component, but Maxwell names depth R or
        // G depending on the format, so a guest reading the depth of a packed
        // format asks for GREEN, which a single-component host image doesn't
        // have. Fold it onto red like Vulkan's ConvertGreenRed does.
        if (VideoCore::Surface::GetFormatType(info.format) !=
            VideoCore::Surface::SurfaceType::ColorTexture) {
            bool folded = false;
            for (SwizzleSource& source : sources) {
                if (source == SwizzleSource::G) {
                    source = SwizzleSource::R;
                    folded = true;
                }
            }
            if (folded) {
                static std::atomic<u32> reported{0};
                if (reported++ < 8) {
                    LOG_WARNING(Render_Metal,
                                "Depth/stencil view asked for green; folding onto red (fmt {})",
                                info.format);
                }
            }
        }
    }
    // Undo whatever the format substitution moved. A red/blue flip isn't enough,
    // A5B5G5R1 against Metal's A1BGR5 swaps A with R and G with B, and A4B4G4R4
    // against ABGR4 is a full reversal.
    switch (fix) {
    case SwizzleFix::None:
        break;
    case SwizzleFix::SwapRedBlue:
        for (SwizzleSource& source : sources) {
            if (source == SwizzleSource::R) {
                source = SwizzleSource::B;
            } else if (source == SwizzleSource::B) {
                source = SwizzleSource::R;
            }
        }
        break;
    case SwizzleFix::SwapGreenRed:
        for (SwizzleSource& source : sources) {
            if (source == SwizzleSource::R) {
                source = SwizzleSource::G;
            } else if (source == SwizzleSource::G) {
                source = SwizzleSource::R;
            }
        }
        break;
    case SwizzleFix::SwapSpecial:
        for (SwizzleSource& source : sources) {
            switch (source) {
            case SwizzleSource::A:
                source = SwizzleSource::R;
                break;
            case SwizzleSource::R:
                source = SwizzleSource::A;
                break;
            case SwizzleSource::G:
                source = SwizzleSource::B;
                break;
            case SwizzleSource::B:
                source = SwizzleSource::G;
                break;
            default:
                break;
            }
        }
        break;
    case SwizzleFix::Reverse:
        std::ranges::reverse(sources);
        break;
    }
    return MTL::TextureSwizzleChannels{ToSwizzle(sources[0]), ToSwizzle(sources[1]),
                                       ToSwizzle(sources[2]), ToSwizzle(sources[3])};
}

// The Metal format a shader-visible view should sample through. An image and a
// view over it don't have to agree, the shared cache reuses one image for any
// view in the same view class, so a surface rendered as RGBA can later be
// sampled through a BGRA header and the image's format would swap red and blue.
// Reinterpreting only works between formats of the same texel size, anything
// else falls back to the image's format.
// Metal aborts the process if newTextureView crosses pixel-format view classes
// (e.g. depth/stencil -> color, packed 16-bit -> R16, BC1 -> BC4, or different
// texel sizes). Classify each format into its Metal view class before creating
// any reinterpreted view.
[[nodiscard]] u32 MetalPixelFormatViewClass(MTL::PixelFormat format) noexcept {
    switch (format) {
    // 8-bit ordinary
    case MTL::PixelFormatA8Unorm:
    case MTL::PixelFormatR8Unorm:
    case MTL::PixelFormatR8Unorm_sRGB:
    case MTL::PixelFormatR8Snorm:
    case MTL::PixelFormatR8Uint:
    case MTL::PixelFormatR8Sint:
        return 1;
    // 16-bit ordinary
    case MTL::PixelFormatR16Unorm:
    case MTL::PixelFormatR16Snorm:
    case MTL::PixelFormatR16Uint:
    case MTL::PixelFormatR16Sint:
    case MTL::PixelFormatR16Float:
    case MTL::PixelFormatRG8Unorm:
    case MTL::PixelFormatRG8Unorm_sRGB:
    case MTL::PixelFormatRG8Snorm:
    case MTL::PixelFormatRG8Uint:
    case MTL::PixelFormatRG8Sint:
        return 2;
    // 16-bit packed (each is its own view class in Metal)
    case MTL::PixelFormatB5G6R5Unorm:
        return 100;
    case MTL::PixelFormatA1BGR5Unorm:
        return 101;
    case MTL::PixelFormatABGR4Unorm:
        return 102;
    case MTL::PixelFormatBGR5A1Unorm:
        return 103;
    // 32-bit ordinary
    case MTL::PixelFormatR32Uint:
    case MTL::PixelFormatR32Sint:
    case MTL::PixelFormatR32Float:
    case MTL::PixelFormatRG16Unorm:
    case MTL::PixelFormatRG16Snorm:
    case MTL::PixelFormatRG16Uint:
    case MTL::PixelFormatRG16Sint:
    case MTL::PixelFormatRG16Float:
    case MTL::PixelFormatRGBA8Unorm:
    case MTL::PixelFormatRGBA8Unorm_sRGB:
    case MTL::PixelFormatRGBA8Snorm:
    case MTL::PixelFormatRGBA8Uint:
    case MTL::PixelFormatRGBA8Sint:
    case MTL::PixelFormatBGRA8Unorm:
    case MTL::PixelFormatBGRA8Unorm_sRGB:
    case MTL::PixelFormatRGB10A2Unorm:
    case MTL::PixelFormatRGB10A2Uint:
    case MTL::PixelFormatRG11B10Float:
    case MTL::PixelFormatRGB9E5Float:
    case MTL::PixelFormatBGR10A2Unorm:
        return 3;
    // 64-bit ordinary
    case MTL::PixelFormatRG32Uint:
    case MTL::PixelFormatRG32Sint:
    case MTL::PixelFormatRG32Float:
    case MTL::PixelFormatRGBA16Unorm:
    case MTL::PixelFormatRGBA16Snorm:
    case MTL::PixelFormatRGBA16Uint:
    case MTL::PixelFormatRGBA16Sint:
    case MTL::PixelFormatRGBA16Float:
        return 4;
    // 128-bit ordinary
    case MTL::PixelFormatRGBA32Uint:
    case MTL::PixelFormatRGBA32Sint:
    case MTL::PixelFormatRGBA32Float:
        return 5;
    // Compressed BCn (only sRGB <-> non-sRGB within the same BC format)
    case MTL::PixelFormatBC1_RGBA:
    case MTL::PixelFormatBC1_RGBA_sRGB:
        return 200;
    case MTL::PixelFormatBC2_RGBA:
    case MTL::PixelFormatBC2_RGBA_sRGB:
        return 201;
    case MTL::PixelFormatBC3_RGBA:
    case MTL::PixelFormatBC3_RGBA_sRGB:
        return 202;
    case MTL::PixelFormatBC4_RUnorm:
    case MTL::PixelFormatBC4_RSnorm:
        return 203;
    case MTL::PixelFormatBC5_RGUnorm:
    case MTL::PixelFormatBC5_RGSnorm:
        return 204;
    case MTL::PixelFormatBC6H_RGBFloat:
    case MTL::PixelFormatBC6H_RGBUfloat:
        return 205;
    case MTL::PixelFormatBC7_RGBAUnorm:
    case MTL::PixelFormatBC7_RGBAUnorm_sRGB:
        return 206;
    default:
        // Depth/stencil, ASTC, and any other format: only compatible with itself
        // (except explicit X32_Stencil8 / X24_Stencil8 handled in StencilView).
        return 10000u + static_cast<u32>(format);
    }
}

[[nodiscard]] bool AreMetalPixelFormatsViewCompatible(MTL::PixelFormat base,
                                                      MTL::PixelFormat view) noexcept {
    if (base == view) {
        return true;
    }
    return MetalPixelFormatViewClass(base) == MetalPixelFormatViewClass(view);
}

[[nodiscard]] MTL::PixelFormat StripSrgbFormat(MTL::PixelFormat format) noexcept {
    switch (format) {
    case MTL::PixelFormatR8Unorm_sRGB:
        return MTL::PixelFormatR8Unorm;
    case MTL::PixelFormatRG8Unorm_sRGB:
        return MTL::PixelFormatRG8Unorm;
    case MTL::PixelFormatRGBA8Unorm_sRGB:
        return MTL::PixelFormatRGBA8Unorm;
    case MTL::PixelFormatBGRA8Unorm_sRGB:
        return MTL::PixelFormatBGRA8Unorm;
    default:
        return format;
    }
}

[[nodiscard]] MTL::PixelFormat ViewSampleFormat(const FormatInfo& view_format,
                                                PixelFormat view_pixel_format,
                                                PixelFormat image_pixel_format,
                                                MTL::PixelFormat image_format) {
    using VideoCore::Surface::BytesPerBlock;
    using VideoCore::Surface::DefaultBlockHeight;
    using VideoCore::Surface::DefaultBlockWidth;

    if (view_pixel_format == image_pixel_format ||
        view_format.support == FormatSupport::Unsupported) {
        return image_format;
    }
    // An image whose texels got widened on upload isn't the format the guest
    // named, so the size test below would compare the wrong thing and the view
    // would read every texel at the wrong stride.
    if (VideoCommon::IsPixelFormatExpanded(image_pixel_format) ||
        VideoCommon::IsPixelFormatExpanded(view_pixel_format)) {
        return image_format;
    }
    if (BytesPerBlock(view_pixel_format) != BytesPerBlock(image_pixel_format) ||
        DefaultBlockWidth(view_pixel_format) != DefaultBlockWidth(image_pixel_format) ||
        DefaultBlockHeight(view_pixel_format) != DefaultBlockHeight(image_pixel_format)) {
        return image_format;
    }
    if (!AreMetalPixelFormatsViewCompatible(image_format, view_format.format)) {
        return image_format;
    }
    static std::atomic<u32> reported{0};
    if (reported++ < 16) {
        LOG_INFO(Render_Metal, "Sampling image format {} through view format {}",
                 image_pixel_format, view_pixel_format);
    }
    return view_format.format;
}

// Guest pixel format for a shader's declared storage-image format.
[[nodiscard]] PixelFormat StorageFormatOf(Shader::ImageFormat format) {
    switch (format) {
    case Shader::ImageFormat::R8_UINT:
        return PixelFormat::R8_UINT;
    case Shader::ImageFormat::R8_SINT:
        return PixelFormat::R8_SINT;
    case Shader::ImageFormat::R16_UINT:
        return PixelFormat::R16_UINT;
    case Shader::ImageFormat::R16_SINT:
        return PixelFormat::R16_SINT;
    case Shader::ImageFormat::R32_UINT:
        return PixelFormat::R32_UINT;
    case Shader::ImageFormat::R32G32_UINT:
        return PixelFormat::R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT:
        return PixelFormat::R32G32B32A32_UINT;
    case Shader::ImageFormat::Typeless:
        break;
    }
    return PixelFormat::Invalid;
}

[[nodiscard]] u32 NumColorChannels(MTL::PixelFormat format) {
    switch (format) {
    case MTL::PixelFormatA8Unorm:
    case MTL::PixelFormatR8Unorm:
    case MTL::PixelFormatR8Unorm_sRGB:
    case MTL::PixelFormatR8Snorm:
    case MTL::PixelFormatR8Uint:
    case MTL::PixelFormatR8Sint:
    case MTL::PixelFormatR16Unorm:
    case MTL::PixelFormatR16Snorm:
    case MTL::PixelFormatR16Uint:
    case MTL::PixelFormatR16Sint:
    case MTL::PixelFormatR16Float:
    case MTL::PixelFormatR32Uint:
    case MTL::PixelFormatR32Sint:
    case MTL::PixelFormatR32Float:
        return 1;
    case MTL::PixelFormatRG8Unorm:
    case MTL::PixelFormatRG8Unorm_sRGB:
    case MTL::PixelFormatRG8Snorm:
    case MTL::PixelFormatRG8Uint:
    case MTL::PixelFormatRG8Sint:
    case MTL::PixelFormatRG16Unorm:
    case MTL::PixelFormatRG16Snorm:
    case MTL::PixelFormatRG16Uint:
    case MTL::PixelFormatRG16Sint:
    case MTL::PixelFormatRG16Float:
    case MTL::PixelFormatRG32Uint:
    case MTL::PixelFormatRG32Sint:
    case MTL::PixelFormatRG32Float:
        return 2;
    case MTL::PixelFormatB5G6R5Unorm:
    case MTL::PixelFormatRG11B10Float:
    case MTL::PixelFormatRGB9E5Float:
        return 3;
    default:
        return 4;
    }
}

/// Like Vulkan: a swizzle naming the values of the channels the format lacks (zero, alpha
/// one) reads like the identity, which is what a framebuffer fetch of the attachment gives.
[[nodiscard]] bool ReadsLikeIdentity(const MTL::TextureSwizzleChannels& swizzle,
                                     MTL::PixelFormat format) {
    const u32 num_channels = NumColorChannels(format);
    const std::array<MTL::TextureSwizzle, 4> channels{swizzle.red, swizzle.green, swizzle.blue,
                                                      swizzle.alpha};
    for (u32 component = 0; component < 4; ++component) {
        const auto own = static_cast<MTL::TextureSwizzle>(MTL::TextureSwizzleRed + component);
        const auto lacked = component == 3 ? MTL::TextureSwizzleOne : MTL::TextureSwizzleZero;
        if (channels[component] != own &&
            (component < num_channels || channels[component] != lacked)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool IsIdentitySwizzle(const MTL::TextureSwizzleChannels& swizzle) {
    return swizzle.red == MTL::TextureSwizzleRed && swizzle.green == MTL::TextureSwizzleGreen &&
           swizzle.blue == MTL::TextureSwizzleBlue && swizzle.alpha == MTL::TextureSwizzleAlpha;
}

// Metal's texture view classes. A view can only reinterpret a texture as another
// type in the same class, zero means it can't be viewed at all.
constexpr int ViewClass(MTL::TextureType type) {
    switch (type) {
    case MTL::TextureType1D:
    case MTL::TextureType1DArray:
        return 1;
    case MTL::TextureType2D:
    case MTL::TextureType2DArray:
    case MTL::TextureTypeCube:
    case MTL::TextureTypeCubeArray:
        return 2;
    case MTL::TextureType2DMultisample:
    case MTL::TextureType2DMultisampleArray:
        return 3;
    case MTL::TextureType3D:
        return 4;
    case MTL::TextureTypeTextureBuffer:
        return 5;
    default:
        return 0;
    }
}

// The guest's border colour rounded to the three Metal gives us (transparent
// black, opaque black, opaque white). Thresholds are the same as the Vulkan
// fallback for when VK_EXT_custom_border_color isn't there.
MTL::SamplerBorderColor BorderColorFor(const std::array<f32, 4>& color) {
    if (color == std::array<f32, 4>{0.0f, 0.0f, 0.0f, 0.0f}) {
        return MTL::SamplerBorderColorTransparentBlack;
    }
    if (color == std::array<f32, 4>{0.0f, 0.0f, 0.0f, 1.0f}) {
        return MTL::SamplerBorderColorOpaqueBlack;
    }
    if (color == std::array<f32, 4>{1.0f, 1.0f, 1.0f, 1.0f}) {
        return MTL::SamplerBorderColorOpaqueWhite;
    }
    if (color[0] + color[1] + color[2] > 1.35f) {
        return MTL::SamplerBorderColorOpaqueWhite;
    }
    return color[3] > 0.5f ? MTL::SamplerBorderColorOpaqueBlack
                           : MTL::SamplerBorderColorTransparentBlack;
}

// Everything a guest image might get used for. Metal wants usage declared up
// front and getting it wrong is a validation failure, not a slow path, so we're
// permissive here. It only costs us some lossless-compression opportunities.
MTL::TextureUsage UsageFor(const ImageInfo& info, const FormatInfo& format_info) {
    MTL::TextureUsage usage = MTL::TextureUsageShaderRead;

    if (format_info.attachable) {
        usage |= MTL::TextureUsageRenderTarget;
    }
    if (format_info.storage) {
        usage |= MTL::TextureUsageShaderWrite;
    }
    // Image atomics compile to native texture atomics, which Metal allows on 32-bit
    // integer textures only.
    using VideoCore::Surface::PixelFormat;
    if (info.format == PixelFormat::R32_UINT || info.format == PixelFormat::R32_SINT) {
        usage |= MTL::TextureUsageShaderAtomic;
    }
    // Views with a different pixel format need this declared at creation time.
    usage |= MTL::TextureUsagePixelFormatView;

    return usage;
}

// Whether a format carries depth and stencil in one texture. A blit treats the
// two aspects as separate planes and rejects a buffer copy that doesn't pick
// one, MTLBlitOptionNone asserts in validation and is UB without it.
bool IsCombinedDepthStencil(MTL::PixelFormat format) {
    switch (format) {
    case MTL::PixelFormatDepth32Float_Stencil8:
    case MTL::PixelFormatDepth24Unorm_Stencil8:
    case MTL::PixelFormatX32_Stencil8:
    case MTL::PixelFormatX24_Stencil8:
        return true;
    default:
        return false;
    }
}

MTL::Origin ToOrigin(const VideoCommon::Offset3D& offset) {
    return MTL::Origin::Make(static_cast<NS::UInteger>(offset.x),
                             static_cast<NS::UInteger>(offset.y),
                             static_cast<NS::UInteger>(offset.z));
}

MTL::Size ToSize(const VideoCommon::Extent3D& extent) {
    return MTL::Size::Make(static_cast<NS::UInteger>(extent.width),
                           static_cast<NS::UInteger>(extent.height),
                           static_cast<NS::UInteger>(extent.depth));
}

} // Anonymous namespace

// ---------------------------------------------------------------------------
// TextureCacheRuntime
// ---------------------------------------------------------------------------

TextureCacheRuntime::TextureCacheRuntime(const Device& device_, Scheduler& scheduler_,
                                         MemoryAllocator& memory_allocator_,
                                         StagingBufferPool& staging_buffer_pool_,
                                         BlitImageHelper& blit_image_helper_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      staging_buffer_pool{staging_buffer_pool_}, blit_image_helper{blit_image_helper_},
      resolution{videoSettings.resolution_info} {}

void TextureCacheRuntime::Finish() {
    scheduler.Finish();
}

void TextureCacheRuntime::TickFrame() {
    ++frame_index;
    constexpr u64 TEMP_BUFFER_IDLE_FRAMES = 300;
    if ((frame_index % 60) == 0) {
        for (size_t log2 = 0; log2 < INDEXING_SLOTS; ++log2) {
            if (!temporary_buffers[log2]) {
                continue;
            }
            if (frame_index - temporary_buffer_frames[log2] >= TEMP_BUFFER_IDLE_FRAMES &&
                scheduler.IsFree(temporary_buffer_ticks[log2])) {
                device.RemoveResidentAllocation(temporary_buffers[log2].get());
                temporary_buffers[log2].reset();
            }
        }
    }
    if ((frame_index % 300) == 0) {
        Common::PurgeHostHeap();
    }
}

StagingBufferRef TextureCacheRuntime::UploadStagingBuffer(size_t size, bool deferred) {
    return staging_buffer_pool.Request(size, MemoryUsage::Upload, deferred);
}

StagingBufferRef TextureCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging_buffer_pool.Request(size, MemoryUsage::Download, deferred);
}

void TextureCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    staging_buffer_pool.FreeDeferred(ref);
}

u64 TextureCacheRuntime::GetDeviceLocalMemory() const {
    constexpr u64 GiB = u64{1} << 30;
    const u64 working_set = device.GetRecommendedWorkingSetSize();
    const u64 scale = (std::max<u64>)(1, videoSettings.resolution_info.ScaleUp(1));
    // On unified memory, Metal reports ~75% of system RAM (16-96 GiB) as the
    // working set. Handing that directly to TextureCache pushes its GC thresholds
    // above 7-15+ GiB, disabling eviction while dynamic-resolution titles (e.g.
    // Fast RMX) accumulate thousands of dead render targets. Conversely, capping
    // at 4 GiB collapses minimum_memory to 0 and expected_memory to 1.6 GiB,
    // forcing high-priority GC every frame on ~2 GiB titles (evicting/re-uploading
    // depth targets and hammering CommitResidency).
    const u64 mode_cap = videoSettings.vram_usage_mode == VramUsageMode::Aggressive
                             ? (8 * GiB + scale * GiB)
                             : ((7 * GiB) / 2 + scale * GiB);
    const u64 cap = device.HasUnifiedMemory() ? (std::min)(working_set / 2, mode_cap) : mode_cap;
    return (std::min)(working_set, (std::max)(cap, 2 * GiB));
}

u64 TextureCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDevice()->currentAllocatedSize();
}

bool TextureCacheRuntime::CanReportMemoryUsage() const {
    // False on purpose. Say true and the shared cache swaps its own image
    // accounting for the device-wide currentAllocatedSize(), which its collector
    // can't bring down by evicting images, so it evicts every frame.
    return false;
}

std::optional<size_t> TextureCacheRuntime::GetSamplerHeapBudget() const {
    // Metal imposes no global sampler-object limit worth surfacing here.
    return std::nullopt;
}

MTL::Buffer* TextureCacheRuntime::GetTemporaryBuffer(size_t needed_size) {
    const size_t log2 = static_cast<size_t>(std::bit_width(needed_size - 1));
    if (log2 >= INDEXING_SLOTS) {
        return nullptr;
    }
    if (temporary_buffers[log2]) {
        temporary_buffer_frames[log2] = frame_index;
        temporary_buffer_ticks[log2] = scheduler.CurrentTick() + 1;
        return temporary_buffers[log2].get();
    }

    const size_t size = size_t{1} << log2;
    temporary_buffers[log2] = memory_allocator.CreateBuffer(size, MemoryUsage::DeviceLocal);
    if (temporary_buffers[log2]) {
        temporary_buffers[log2]->setLabel(MTLSTR("eden temp texture buffer"));
        temporary_buffer_frames[log2] = frame_index;
        temporary_buffer_ticks[log2] = scheduler.CurrentTick() + 1;
    }
    return temporary_buffers[log2].get();
}

bool TextureCacheRuntime::IsFormatDitherable(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8B8G8R8_UNORM:
    case PixelFormat::B8G8R8A8_UNORM:
    case PixelFormat::A8B8G8R8_SRGB:
    case PixelFormat::B8G8R8A8_SRGB:
        return true;
    default:
        return false;
    }
}

bool TextureCacheRuntime::IsFormatScalable(PixelFormat format) {
    // Allow any renderable, non-compressed, non-stencil-only format (matching
    // Vulkan's ImageCanRescale). BlitColor handles float, uint, and sint targets
    // (using nearest filtering for integer formats), and DepthToDepth uses nearest.
    const SurfaceType type = VideoCore::Surface::GetFormatType(format);
    if (type == SurfaceType::Stencil) {
        return false;
    }
    if (VideoCore::Surface::DefaultBlockWidth(format) > 1 ||
        VideoCore::Surface::DefaultBlockHeight(format) > 1) {
        return false;
    }
    if (type == SurfaceType::DepthStencil) {
        // Known cost. Metal has no stencil export so the Convert path writes
        // depth only, and a rescaled packed image keeps the destination's old
        // stencil. Still better than clamping every scaled pass to 1x depth.
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            LOG_WARNING(Render_Metal,
                        "Rescaling a packed depth/stencil image; stencil contents are not "
                        "carried across the rescale");
        }
    }

    const FormatInfo info = SurfaceFormat(device, format);
    return info.attachable && info.support != FormatSupport::Unsupported;
}

MTL::Texture* TextureCacheRuntime::GetScratchAttachment(MTL::PixelFormat format, u32 width,
                                                        u32 height, u32 samples) {
    const ScratchAttachmentKey key{format, width, height, samples};
    const auto it = scratch_attachments.find(key);
    if (it != scratch_attachments.end()) {
        return it->second.get();
    }

    constexpr size_t MAX_SCRATCH_ATTACHMENTS = 64;
    if (scratch_attachments.size() >= MAX_SCRATCH_ATTACHMENTS) {
        for (const auto& [old_key, old_tex] : scratch_attachments) {
            if (old_tex) {
                scheduler.KeepAlive(old_tex.get(), old_tex.get());
            }
        }
        scratch_attachments.clear();
    }

    ScopedAutoreleasePool pool;
    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setTextureType(samples > 1 ? MTL::TextureType2DMultisample : MTL::TextureType2D);
    descriptor->setPixelFormat(format);
    descriptor->setWidth(width);
    descriptor->setHeight(height);
    descriptor->setSampleCount(samples);
    descriptor->setMipmapLevelCount(1);
    descriptor->setUsage(MTL::TextureUsageRenderTarget);
    Owned<MTL::Texture> texture =
        memory_allocator.CreateTexture(descriptor, MemoryUsage::Transient);
    descriptor->release();
    if (!texture) {
        return nullptr;
    }
    texture->setLabel(MTLSTR("eden scratch attachment"));
    return scratch_attachments.emplace(key, std::move(texture)).first->second.get();
}

MTL::Texture* TextureCacheRuntime::GetOrCreateTirColorScratch(MTL::Texture* color_image,
                                                              MTL::PixelFormat format, u32 width,
                                                              u32 height, u32 layers, u32 samples) {
    if (color_image == nullptr || width == 0 || height == 0 || samples <= 1) {
        return nullptr;
    }
    const u32 layer_count = (std::max)(1u, layers);
    TirColorScratch& scratch = tir_color_scratch[color_image];
    if (scratch.texture && scratch.format == format && scratch.samples == samples &&
        scratch.layers == layer_count && scratch.width >= width && scratch.height >= height) {
        return scratch.texture.get();
    }
    if (scratch.texture) {
        // Previous passes may still reference the old scratch texture; keep it alive until retired.
        scheduler.KeepAlive(scratch.texture.get(), scratch.texture.get());
        width = (std::max)(width, scratch.width);
        height = (std::max)(height, scratch.height);
    }

    ScopedAutoreleasePool pool;
    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setTextureType(layer_count > 1 ? MTL::TextureType2DMultisampleArray
                                               : MTL::TextureType2DMultisample);
    descriptor->setPixelFormat(format);
    descriptor->setWidth(width);
    descriptor->setHeight(height);
    descriptor->setArrayLength(layer_count);
    descriptor->setSampleCount(samples);
    descriptor->setMipmapLevelCount(1);
    descriptor->setUsage(MTL::TextureUsageRenderTarget);
    Owned<MTL::Texture> texture =
        memory_allocator.CreateTexture(descriptor, MemoryUsage::DeviceLocal);
    descriptor->release();
    if (!texture) {
        LOG_WARNING(Render_Metal, "Could not allocate a {}x{}x{} {}x TIR color scratch", width,
                    height, layer_count, samples);
        return nullptr;
    }
    texture->setLabel(MTLSTR("nxemu TIR color scratch"));
    scratch.texture = std::move(texture);
    scratch.format = format;
    scratch.width = width;
    scratch.height = height;
    scratch.layers = layer_count;
    scratch.samples = samples;

    // Give the persistent TIR scratch defined initial contents before the first LoadAction::Load.
    RenderPassState clear_pass{};
    clear_pass.num_color = 1;
    clear_pass.color[0].texture = scratch.texture.get();
    clear_pass.color[0].load_action = LoadAction::Clear;
    clear_pass.color[0].store_action = StoreAction::Store;
    clear_pass.render_area_width = width;
    clear_pass.render_area_height = height;
    clear_pass.num_layers = layer_count;
    clear_pass.sample_count = samples;
    scheduler.RequestRenderPass(clear_pass);
    scheduler.RequestOutsideRenderPassOperationContext();

    static std::atomic<u32> created{0};
    if (created++ == 0) {
        LOG_INFO(Render_Metal,
                 "Enabled TIR mixed-sample rendering ({}x depth with 1x color via MSAA scratch)",
                 samples);
    }
    return scratch.texture.get();
}

void TextureCacheRuntime::ReleaseTirColorScratch(MTL::Texture* color_image) {
    if (color_image == nullptr) {
        return;
    }
    const auto it = tir_color_scratch.find(color_image);
    if (it != tir_color_scratch.end()) {
        if (it->second.texture) {
            scheduler.KeepAlive(it->second.texture.get(), it->second.texture.get());
        }
        tir_color_scratch.erase(it);
    }
}

void TextureCacheRuntime::BarrierFeedbackLoop() {
    // TextureCache::CheckFeedbackLoop already found a sampled image this pass
    // wrote. On a tiler those writes are still in tile memory until the pass ends.
    scheduler.RequestOutsideRenderPassOperationContext();
}

void TextureCacheRuntime::CopyImage(Image& dst, Image& src,
                                    std::span<const VideoCommon::ImageCopy> copies) {
    dst.MarkGpuHistory();
    src.MarkGpuHistory();
    if (!dst.IsValid() || !src.IsValid() || copies.empty()) {
        return;
    }

    MTL::Texture* const dst_texture = dst.Handle();
    MTL::Texture* const src_texture = src.Handle();
    if (dst_texture == nullptr || src_texture == nullptr) {
        return;
    }

    const u32 src_bpp = VideoCore::Surface::BytesPerBlock(src.info.format);
    const u32 dst_bpp = VideoCore::Surface::BytesPerBlock(dst.info.format);
    const u32 src_bw = (std::max)(VideoCore::Surface::DefaultBlockWidth(src.info.format), 1u);
    const u32 src_bh = (std::max)(VideoCore::Surface::DefaultBlockHeight(src.info.format), 1u);
    const u32 dst_bw = (std::max)(VideoCore::Surface::DefaultBlockWidth(dst.info.format), 1u);
    const u32 dst_bh = (std::max)(VideoCore::Surface::DefaultBlockHeight(dst.info.format), 1u);

    // Formats of different texel size can't be copied texture-to-texture. Clamp and
    // convert source X offsets into destination-texel units (matching Vulkan) before
    // routing through ReinterpretImage.
    if (src_bpp != dst_bpp) {
        const u32 src_unit = src_bpp / src_bw;
        const u32 dst_unit = dst_bpp / dst_bw;
        if (src_unit == 0 || dst_unit == 0) {
            return;
        }
        const u32 dst_granularity = src_unit / std::gcd(src_unit, dst_unit);
        const auto host_size = [this](const Image& image) {
            auto size = image.info.size;
            if (True(image.flags & VideoCommon::ImageFlagBits::Rescaled)) {
                size.width = resolution.ScaleUp(size.width);
                if (image.info.type == VideoCommon::ImageType::e2D) {
                    size.height = resolution.ScaleUp(size.height);
                }
            }
            return size;
        };
        const auto src_size = host_size(src);
        const auto dst_size = host_size(dst);
        boost::container::small_vector<VideoCommon::ImageCopy, 16> reinterpret_copies;
        reinterpret_copies.reserve(copies.size());
        for (auto copy : copies) {
            const auto valid_subresource = [](const auto& sub, const auto& info) {
                return sub.base_level >= 0 && sub.base_level < info.resources.levels &&
                       sub.base_level < 32 && sub.base_layer >= 0 &&
                       sub.base_layer < info.resources.layers && sub.num_layers > 0;
            };
            if (!valid_subresource(copy.src_subresource, src.info) ||
                !valid_subresource(copy.dst_subresource, dst.info)) {
                continue;
            }
            const auto src_mip = VideoCommon::MipSize(src_size, copy.src_subresource.base_level);
            const auto dst_mip = VideoCommon::MipSize(dst_size, copy.dst_subresource.base_level);
            if (copy.src_offset.x < 0 || copy.dst_offset.x < 0 || copy.src_offset.y < 0 ||
                copy.dst_offset.y < 0 || copy.src_offset.z < 0 || copy.dst_offset.z < 0 ||
                static_cast<u32>(copy.src_offset.x) >= src_mip.width ||
                static_cast<u32>(copy.dst_offset.x) >= dst_mip.width ||
                static_cast<u32>(copy.src_offset.y) >= src_mip.height ||
                static_cast<u32>(copy.dst_offset.y) >= dst_mip.height ||
                static_cast<u32>(copy.src_offset.z) >= src_mip.depth ||
                static_cast<u32>(copy.dst_offset.z) >= dst_mip.depth) {
                continue;
            }
            const u64 src_byte_offset = static_cast<u64>(copy.src_offset.x) * src_unit;
            if (src_byte_offset % dst_unit != 0) {
                continue;
            }
            const u64 available_src_bytes =
                static_cast<u64>(src_mip.width - copy.src_offset.x) * src_unit;
            copy.extent.width = (std::min)({
                copy.extent.width,
                static_cast<u32>(available_src_bytes / dst_unit),
                dst_mip.width - static_cast<u32>(copy.dst_offset.x),
            });
            copy.extent.width -= copy.extent.width % dst_granularity;
            copy.extent.height = (std::min)({copy.extent.height,
                                             src_mip.height - static_cast<u32>(copy.src_offset.y),
                                             dst_mip.height - static_cast<u32>(copy.dst_offset.y)});
            copy.extent.depth = (std::min)({copy.extent.depth,
                                            src_mip.depth - static_cast<u32>(copy.src_offset.z),
                                            dst_mip.depth - static_cast<u32>(copy.dst_offset.z)});
            const s32 layers =
                (std::min)({copy.src_subresource.num_layers, copy.dst_subresource.num_layers,
                            src.info.resources.layers - copy.src_subresource.base_layer,
                            dst.info.resources.layers - copy.dst_subresource.base_layer});
            copy.src_subresource.num_layers = layers;
            copy.dst_subresource.num_layers = layers;
            copy.src_offset.x = static_cast<s32>(src_byte_offset / dst_unit);
            if (copy.extent.width == 0 || copy.extent.height == 0 || copy.extent.depth == 0) {
                continue;
            }
            reinterpret_copies.push_back(copy);
        }
        if (!reinterpret_copies.empty()) {
            ReinterpretImage(dst, src, reinterpret_copies);
        }
        return;
    }

    // Compressed <-> uncompressed copies of the same block byte size (e.g. BC3 <-> RGBA32Uint,
    // BC1 <-> RG32Uint) or incompatible Metal format classes cannot use texture-to-texture
    // copyFromTexture; stage through a DeviceLocal buffer instead.
    if (src_bw != dst_bw || src_bh != dst_bh ||
        !AreMetalPixelFormatsViewCompatible(src_texture->pixelFormat(),
                                            dst_texture->pixelFormat())) {
        struct BlockCopyStep {
            VideoCommon::ImageCopy copy;
            u64 offset;
            u32 bytes_per_row;
            u32 bytes_per_image;
            MTL::Size src_size;
            MTL::Size dst_size;
            u32 num_slices;
        };
        const u64 alignment = (std::max<u64>)(16u, src_bpp);
        boost::container::small_vector<BlockCopyStep, 8> steps;
        u64 total = 0;
        for (const VideoCommon::ImageCopy& copy : copies) {
            const u32 blocks_x = (copy.extent.width + src_bw - 1) / src_bw;
            const u32 blocks_y = (copy.extent.height + src_bh - 1) / src_bh;
            if (blocks_x == 0 || blocks_y == 0) {
                continue;
            }
            const u32 bytes_per_row = blocks_x * src_bpp;
            const u32 bytes_per_image = bytes_per_row * blocks_y;
            const u32 dst_mip_w =
                (std::max)(1u, static_cast<u32>(dst_texture->width() >>
                                                copy.dst_subresource.base_level));
            const u32 dst_mip_h =
                (std::max)(1u, static_cast<u32>(dst_texture->height() >>
                                                copy.dst_subresource.base_level));
            if (static_cast<u32>(copy.dst_offset.x) >= dst_mip_w ||
                static_cast<u32>(copy.dst_offset.y) >= dst_mip_h) {
                continue;
            }
            const u32 dst_w = (std::min)(blocks_x * dst_bw,
                                         dst_mip_w - static_cast<u32>(copy.dst_offset.x));
            const u32 dst_h = (std::min)(blocks_y * dst_bh,
                                         dst_mip_h - static_cast<u32>(copy.dst_offset.y));
            const u32 num_layers =
                (std::max)(1u, static_cast<u32>(copy.src_subresource.num_layers));
            const u32 depth = (std::max)(1u, copy.extent.depth);
            total = Common::AlignUp(total, alignment);
            steps.push_back(BlockCopyStep{
                .copy = copy,
                .offset = total,
                .bytes_per_row = bytes_per_row,
                .bytes_per_image = bytes_per_image,
                .src_size = AlignCopySizeToBlock(
                    MTL::Size::Make(copy.extent.width, copy.extent.height, depth),
                    src_texture->pixelFormat()),
                .dst_size = AlignCopySizeToBlock(MTL::Size::Make(dst_w, dst_h, depth),
                                                 dst_texture->pixelFormat()),
                .num_slices = num_layers,
            });
            total += static_cast<u64>(bytes_per_image) * depth * num_layers;
        }
        if (total == 0 || steps.empty()) {
            return;
        }
        const StagingBufferRef ref = staging_buffer_pool.Request(total, MemoryUsage::DeviceLocal);
        if (ref.buffer == nullptr) {
            LOG_WARNING(Render_Metal, "No staging space for cross-format copy ({} bytes)", total);
            return;
        }
        MTL::Buffer* const buffer = ref.buffer;
        const u64 base = ref.offset;
        const MTL::BlitOption src_options = IsCombinedDepthStencil(src_texture->pixelFormat())
                                                ? MTL::BlitOptionDepthFromDepthStencil
                                                : MTL::BlitOptionNone;
        const MTL::BlitOption dst_options = IsCombinedDepthStencil(dst_texture->pixelFormat())
                                                ? MTL::BlitOptionDepthFromDepthStencil
                                                : MTL::BlitOptionNone;
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([src_texture, dst_texture, buffer, base, src_options, dst_options,
                          steps = std::move(steps)](CommandContext& context) {
            BlitEncoder encoder = context.RequestBlit();
            if (encoder == nullptr) {
                return;
            }
            for (const BlockCopyStep& step : steps) {
                const auto& copy = step.copy;
                for (u32 layer = 0; layer < step.num_slices; ++layer) {
                    const u64 offset = base + step.offset +
                                       static_cast<u64>(layer) * step.bytes_per_image *
                                           step.src_size.depth;
                    encoder->copyFromTexture(src_texture, copy.src_subresource.base_layer + layer,
                                             copy.src_subresource.base_level,
                                             ToOrigin(copy.src_offset), step.src_size, buffer,
                                             offset, step.bytes_per_row, step.bytes_per_image,
                                             src_options);
                }
            }
            encoder->barrierAfterCopies();
            for (const BlockCopyStep& step : steps) {
                const auto& copy = step.copy;
                for (u32 layer = 0; layer < step.num_slices; ++layer) {
                    const u64 offset = base + step.offset +
                                       static_cast<u64>(layer) * step.bytes_per_image *
                                           step.dst_size.depth;
                    encoder->copyFromBuffer(buffer, offset, step.bytes_per_row,
                                            step.bytes_per_image, step.dst_size, dst_texture,
                                            copy.dst_subresource.base_layer + layer,
                                            copy.dst_subresource.base_level,
                                            ToOrigin(copy.dst_offset), dst_options);
                }
            }
        });
        return;
    }

    boost::container::small_vector<VideoCommon::ImageCopy, 8> owned(copies.begin(), copies.end());

    // Ends the pass on the recording side too. RequestBlit closes the render
    // encoder on the worker, but without this the recording thread's mirror
    // still thinks a pass is open and every later check of that is wrong.
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dst_texture, src_texture,
                      copies = std::move(owned)](CommandContext& context) {
        BlitEncoder encoder = context.RequestBlit();
        if (encoder == nullptr) {
            return;
        }
        const bool src_is_3d = src_texture->textureType() == MTL::TextureType3D;
        const bool dst_is_3d = dst_texture->textureType() == MTL::TextureType3D;
        for (const VideoCommon::ImageCopy& copy : copies) {
            if (src_is_3d && dst_is_3d) {
                encoder->copyFromTexture(
                    src_texture, 0, copy.src_subresource.base_level, ToOrigin(copy.src_offset),
                    AlignCopySizeToBlock(ToSize(copy.extent), src_texture->pixelFormat()),
                    dst_texture, 0, copy.dst_subresource.base_level, ToOrigin(copy.dst_offset));
                continue;
            }
            const MTL::Size slice_size = AlignCopySizeToBlock(
                MTL::Size::Make(copy.extent.width, copy.extent.height, 1),
                src_texture->pixelFormat());
            if (src_is_3d != dst_is_3d) {
                // Copying between a 3D volume and a 2D/2D-array texture requires
                // depth == 1 per call, advancing origin.z on the 3D side and slice
                // index on the 2D-array side.
                const u32 slices = (std::max)(
                    {1u, copy.extent.depth,
                     static_cast<u32>(src_is_3d ? copy.dst_subresource.num_layers
                                                : copy.src_subresource.num_layers)});
                for (u32 s = 0; s < slices; ++s) {
                    const u32 src_slice = src_is_3d ? 0u : (copy.src_subresource.base_layer + s);
                    const u32 dst_slice = dst_is_3d ? 0u : (copy.dst_subresource.base_layer + s);
                    const MTL::Origin src_origin = MTL::Origin::Make(
                        copy.src_offset.x, copy.src_offset.y,
                        src_is_3d ? (copy.src_offset.z + s) : 0u);
                    const MTL::Origin dst_origin = MTL::Origin::Make(
                        copy.dst_offset.x, copy.dst_offset.y,
                        dst_is_3d ? (copy.dst_offset.z + s) : 0u);
                    encoder->copyFromTexture(src_texture, src_slice,
                                             copy.src_subresource.base_level, src_origin,
                                             slice_size, dst_texture, dst_slice,
                                             copy.dst_subresource.base_level, dst_origin);
                }
                continue;
            }
            const u32 num_layers =
                (std::max)(1u, static_cast<u32>(copy.src_subresource.num_layers));
            for (u32 layer = 0; layer < num_layers; ++layer) {
                encoder->copyFromTexture(
                    src_texture, copy.src_subresource.base_layer + layer,
                    copy.src_subresource.base_level, ToOrigin(copy.src_offset), slice_size,
                    dst_texture, copy.dst_subresource.base_layer + layer,
                    copy.dst_subresource.base_level, ToOrigin(copy.dst_offset));
            }
        }
    });
}

namespace {
/// Builds a load+resolve render pass targeting `dst` from a 2DMultisample `src`,
/// supporting color, depth-only, and packed depth/stencil formats while preserving `src`.
RenderPassState BuildMsaaResolvePass(MTL::Texture* src, MTL::Texture* dst) {
    const MTL::PixelFormat fmt = src->pixelFormat();
    const bool is_depth = fmt == MTL::PixelFormatDepth16Unorm ||
                          fmt == MTL::PixelFormatDepth32Float ||
                          fmt == MTL::PixelFormatDepth24Unorm_Stencil8 ||
                          fmt == MTL::PixelFormatDepth32Float_Stencil8;
    const bool has_stencil = fmt == MTL::PixelFormatDepth24Unorm_Stencil8 ||
                             fmt == MTL::PixelFormatDepth32Float_Stencil8;

    RenderPassState pass{};
    pass.render_area_width = static_cast<u32>(src->width());
    pass.render_area_height = static_cast<u32>(src->height());
    pass.sample_count = static_cast<u32>(src->sampleCount());
    if (is_depth) {
        pass.depth_stencil.texture = src;
        pass.depth_stencil.resolve_texture = dst;
        pass.depth_stencil.depth_load = LoadAction::Load;
        pass.depth_stencil.depth_store = StoreAction::Resolve;
        pass.depth_stencil.has_stencil = has_stencil;
        if (has_stencil) {
            pass.depth_stencil.stencil_load = LoadAction::Load;
            pass.depth_stencil.stencil_store = StoreAction::Resolve;
            pass.depth_stencil.stencil_resolve_texture = dst;
        }
    } else if (fmt == MTL::PixelFormatStencil8) {
        pass.depth_stencil.texture = src;
        pass.depth_stencil.has_stencil = true;
        pass.depth_stencil.stencil_load = LoadAction::Load;
        pass.depth_stencil.stencil_store = StoreAction::Resolve;
        pass.depth_stencil.stencil_resolve_texture = dst;
    } else {
        pass.num_color = 1;
        pass.color[0].texture = src;
        pass.color[0].resolve_texture = dst;
        pass.color[0].load_action = LoadAction::Load;
        pass.color[0].store_action = StoreAction::Resolve;
    }
    return pass;
}
} // Anonymous namespace

void TextureCacheRuntime::CopyImageMSAA(Image& dst, Image& src,
                                        std::span<const VideoCommon::ImageCopy> copies) {
    dst.MarkGpuHistory();
    src.MarkGpuHistory();
    MTL::Texture* const src_texture = src.Handle();
    MTL::Texture* const dst_texture = dst.Handle();
    if (src_texture == nullptr || dst_texture == nullptr) {
        return;
    }

    // A blit encoder can't change sample count. Resolving many samples to one is
    // a render pass store action, so we open a pass that loads and resolves and
    // draw nothing. The other direction has no fixed-function form.
    if (src.Samples() > 1 && dst.Samples() == 1) {
        // Maxwell stores an MSAA image in guest memory as a spatial sample grid
        // (e.g. 1920x1080 4x aliases a 3840x2160 1x view). Unpack every sample
        // losslessly when the destination has the expanded sample-grid dimensions,
        // rather than trying to hardware-resolve into a mismatched size.
        if (dst_texture->width() != src_texture->width() ||
            dst_texture->height() != src_texture->height()) {
            blit_image_helper.UnpackFromMultisample(dst_texture, src_texture);
            static std::atomic<u32> unpacked{0};
            if (unpacked++ == 0) {
                LOG_INFO(Render_Metal,
                         "Unpacking multisampled images into Maxwell sample-grid 1x textures");
            }
            return;
        }
        // A store-action resolve always covers the WHOLE attachment, so we can
        // only write straight into the destination if the guest asked for the
        // whole image. Anything else needs a scratch target and a region copy.
        const bool whole_image =
            copies.empty() ||
            (copies.size() == 1 && copies[0].src_offset.x == 0 && copies[0].src_offset.y == 0 &&
             copies[0].dst_offset.x == 0 && copies[0].dst_offset.y == 0 &&
             copies[0].src_subresource.base_level == 0 &&
             copies[0].dst_subresource.base_level == 0 &&
             copies[0].extent.width == static_cast<u32>(src_texture->width()) &&
             copies[0].extent.height == static_cast<u32>(src_texture->height()));

        Owned<MTL::Texture> scratch;
        MTL::Texture* resolve_target = dst_texture;
        if (!whole_image) {
            ScopedAutoreleasePool pool;
            MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
            descriptor->setTextureType(MTL::TextureType2D);
            descriptor->setPixelFormat(src_texture->pixelFormat());
            descriptor->setWidth(src_texture->width());
            descriptor->setHeight(src_texture->height());
            descriptor->setMipmapLevelCount(1);
            descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
            scratch = memory_allocator.CreateTexture(descriptor, MemoryUsage::DeviceLocal);
            descriptor->release();
            if (!scratch) {
                // Don't fall back to the whole-image resolve, it would clobber
                // the regions the guest didn't name.
                LOG_WARNING(Render_Metal, "No scratch target to resolve {} MSAA region(s)",
                            copies.size());
                return;
            }
            scratch->setLabel(MTLSTR("eden MSAA region resolve"));
            resolve_target = scratch.get();
        }

        const RenderPassState pass = BuildMsaaResolvePass(src_texture, resolve_target);
        scheduler.RequestRenderPass(pass);
        // Closed right away, the resolve is the store action and not a draw.
        scheduler.RequestOutsideRenderPassOperationContext();

        static std::atomic<u32> resolved{0};
        if (resolved++ == 0) {
            LOG_INFO(Render_Metal, "Resolving multisampled images through a store action");
        }

        if (whole_image) {
            return;
        }

        // Now move only what was asked for. The resolved copy is single-sample
        // and single-layer, so every region reads slice 0 level 0 at the source
        // offset the guest named.
        MTL::Texture* const keep = scratch.get();
        boost::container::small_vector<VideoCommon::ImageCopy, 8> owned(copies.begin(),
                                                                       copies.end());
        // Named as an allocation too so the sweep drops it from the residency
        // set as well as releasing it. Those entries are wired, they can't be
        // compressed or evicted, so a stale one costs real physical memory.
        // KeepAlive holds it until the GPU is past this tick, which covers the
        // command recorded below.
        scheduler.KeepAlive(keep, keep);
        scheduler.Record([dst_texture, keep, copies = std::move(owned)](CommandContext& context) {
            BlitEncoder encoder = context.RequestBlit();
            if (encoder == nullptr) {
                return;
            }
            for (const VideoCommon::ImageCopy& copy : copies) {
                const MTL::Size size = AlignCopySizeToBlock(
                    MTL::Size::Make(copy.extent.width, copy.extent.height, 1),
                    keep->pixelFormat());
                encoder->copyFromTexture(keep, 0, 0, ToOrigin(copy.src_offset), size, dst_texture,
                                         copy.dst_subresource.base_layer,
                                         copy.dst_subresource.base_level,
                                         ToOrigin(copy.dst_offset));
            }
        });
        return;
    }

    if (src.Samples() == dst.Samples()) {
        // Same sample count after all, an ordinary copy does it.
        CopyImage(dst, src, copies);
        return;
    }

    if (src.Samples() == 1 && dst.Samples() > 1) {
        // A blit can't write into a multisampled texture, so the expand is a
        // draw whose per-sample invocations all read the same source texel.
        // Only a whole-image request can use the discard-and-draw-once form,
        // regions have to be expanded one by one or the rest gets discarded.
        const bool whole_image =
            copies.empty() ||
            (copies.size() == 1 && copies[0].src_offset.x == 0 && copies[0].src_offset.y == 0 &&
             copies[0].dst_offset.x == 0 && copies[0].dst_offset.y == 0 &&
             copies[0].extent.width == static_cast<u32>(dst_texture->width()) &&
             copies[0].extent.height == static_cast<u32>(dst_texture->height()));

        if (whole_image) {
            blit_image_helper.ExpandToMultisample(dst_texture, src_texture);
        } else {
            boost::container::small_vector<BlitImageHelper::ExpandRegion, 8> regions;
            regions.reserve(copies.size());
            for (const VideoCommon::ImageCopy& copy : copies) {
                regions.push_back(BlitImageHelper::ExpandRegion{
                    static_cast<u32>(copy.src_offset.x), static_cast<u32>(copy.src_offset.y),
                    static_cast<u32>(copy.dst_offset.x), static_cast<u32>(copy.dst_offset.y),
                    copy.extent.width, copy.extent.height});
            }
            blit_image_helper.ExpandToMultisample(dst_texture, src_texture, regions);
        }
        static std::atomic<u32> expanded{0};
        if (expanded++ == 0) {
            LOG_INFO(Render_Metal, "Expanding single-sample images into multisampled ones");
        }
        return;
    }

    static std::atomic<u32> reported{0};
    if (reported++ < 4) {
        LOG_WARNING(Render_Metal, "Cannot spread {} sample(s) across {}", src.Samples(),
                    dst.Samples());
    }
}

bool TextureCacheRuntime::ShouldReinterpret(Image& dst, Image& src) {
    // Metal texture views can reinterpret between formats of the same bit width
    // if the texture was created with PixelFormatView usage, which ours are.
    // Anything crossing bit widths needs a real conversion.
    if (VideoCore::Surface::BytesPerBlock(dst.info.format) !=
        VideoCore::Surface::BytesPerBlock(src.info.format)) {
        return true;
    }

    // Depth-format copies stay on the blit path on purpose, we tried sending
    // them through ReinterpretImage and reverted it.
    return false;
}

void TextureCacheRuntime::ReinterpretImage(Image& dst, Image& src,
                                           std::span<const VideoCommon::ImageCopy> copies) {
    dst.MarkGpuHistory();
    src.MarkGpuHistory();
    // Reinterpreting across bit widths needs no shader, we push the bytes out
    // through a buffer and read them back with the destination's layout. The
    // source rect scales by the ratio of the two formats' bytes per texel.
    MTL::Texture* const src_texture = src.Handle();
    MTL::Texture* const dst_texture = dst.Handle();
    if (src_texture == nullptr || dst_texture == nullptr || copies.empty()) {
        return;
    }

    const u32 src_block_width = VideoCore::Surface::DefaultBlockWidth(src.info.format);
    const u32 dst_block_width = VideoCore::Surface::DefaultBlockWidth(dst.info.format);
    const u32 dst_block_height = VideoCore::Surface::DefaultBlockHeight(dst.info.format);
    const u32 src_bytes_per_block = VideoCore::Surface::BytesPerBlock(src.info.format);
    const u32 dst_bytes_per_block = VideoCore::Surface::BytesPerBlock(dst.info.format);
    const u32 bpp_in = src_bytes_per_block / (std::max)(src_block_width, 1u);
    const u32 bpp_out = dst_bytes_per_block / (std::max)(dst_block_width, 1u);
    if (bpp_in == 0 || bpp_out == 0) {
        return;
    }

    // One staging region per copy, laid out back to back in the destination's
    // own block layout.
    struct Step {
        VideoCommon::ImageCopy copy;
        u64 offset;
        u32 bytes_per_row;
        u32 bytes_per_image;
        u32 src_x;
        u32 src_width;
    };
    const u64 alignment = (std::max<u64>)({16u, src_bytes_per_block, dst_bytes_per_block});
    boost::container::small_vector<Step, 8> steps;
    u64 total = 0;
    for (const VideoCommon::ImageCopy& copy : copies) {
        const u32 blocks_across =
            (copy.extent.width + dst_block_width - 1) / dst_block_width;
        const u32 blocks_down = (copy.extent.height + dst_block_height - 1) / dst_block_height;
        const u32 bytes_per_row = blocks_across * dst_bytes_per_block;
        const u32 bytes_per_image = bytes_per_row * blocks_down;
        total = Common::AlignUp(total, alignment);
        steps.push_back(Step{
            .copy = copy,
            .offset = total,
            .bytes_per_row = bytes_per_row,
            .bytes_per_image = bytes_per_image,
            .src_x = (bpp_out * static_cast<u32>(copy.src_offset.x)) / bpp_in,
            .src_width = (bpp_out * copy.extent.width) / bpp_in,
        });
        total += static_cast<u64>(bytes_per_image) *
                 static_cast<u64>((std::max)(static_cast<s32>(copy.extent.depth) *
                                                 copy.src_subresource.num_layers,
                                             1));
    }
    if (total == 0) {
        return;
    }

    const StagingBufferRef ref = staging_buffer_pool.Request(total, MemoryUsage::DeviceLocal);
    if (ref.buffer == nullptr) {
        LOG_WARNING(Render_Metal, "No staging space to reinterpret a {} byte image", total);
        return;
    }

    MTL::Buffer* const buffer = ref.buffer;
    const u64 base = ref.offset;
    // See Image::UploadMemory, a copy touching a combined depth/stencil texture
    // has to name the aspect it means, in both directions.
    const MTL::BlitOption src_options = IsCombinedDepthStencil(src_texture->pixelFormat())
                                            ? MTL::BlitOptionDepthFromDepthStencil
                                            : MTL::BlitOptionNone;
    const MTL::BlitOption dst_options = IsCombinedDepthStencil(dst_texture->pixelFormat())
                                            ? MTL::BlitOptionDepthFromDepthStencil
                                            : MTL::BlitOptionNone;
    // Ends the pass on the recording side too. RequestBlit closes the render
    // encoder on the worker, but without this the recording thread's mirror
    // still thinks a pass is open and every later check of that is wrong.
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([src_texture, dst_texture, buffer, base, src_options, dst_options,
                      steps = std::move(steps)](CommandContext& context) {
        BlitEncoder encoder = context.RequestBlit();
        if (encoder == nullptr) {
            return;
        }
        // Both halves share one encoder. Every texture-to-buffer copy is
        // recorded before any buffer-to-texture one, with a barrier in between
        // so the reads see the writes.
        for (const Step& step : steps) {
            const auto& copy = step.copy;
            for (s32 layer = 0; layer < copy.src_subresource.num_layers; ++layer) {
                const u64 offset = base + step.offset +
                                   static_cast<u64>(layer) * step.bytes_per_image;
                encoder->copyFromTexture(
                    src_texture, copy.src_subresource.base_layer + layer,
                    copy.src_subresource.base_level,
                    MTL::Origin::Make(step.src_x, copy.src_offset.y, copy.src_offset.z),
                    MTL::Size::Make(step.src_width, copy.extent.height, copy.extent.depth),
                    buffer, offset, step.bytes_per_row, step.bytes_per_image, src_options);
            }
        }
        // The staging buffer is written above and read below. Without this the
        // read-back can see the texture's pre-copy contents.
        encoder->barrierAfterCopies();
        for (const Step& step : steps) {
            const auto& copy = step.copy;
            for (s32 layer = 0; layer < copy.dst_subresource.num_layers; ++layer) {
                const u64 offset = base + step.offset +
                                   static_cast<u64>(layer) * step.bytes_per_image;
                encoder->copyFromBuffer(
                    buffer, offset, step.bytes_per_row, step.bytes_per_image,
                    MTL::Size::Make(copy.extent.width, copy.extent.height, copy.extent.depth),
                    dst_texture, copy.dst_subresource.base_layer + layer,
                    copy.dst_subresource.base_level, ToOrigin(copy.dst_offset), dst_options);
            }
        }
    });
}

void TextureCacheRuntime::ConvertImage(Framebuffer* dst, ImageView& dst_view,
                                       ImageView& src_view) {
    if (dst == nullptr) {
        return;
    }
    const RenderPassState& pass = dst->GetRenderPassState();
    MTL::Texture* const src = src_view.Handle(Shader::TextureType::Color2D);
    if (src == nullptr) {
        return;
    }

    const auto dst_type = VideoCore::Surface::GetFormatType(dst_view.format);
    const auto src_type = VideoCore::Surface::GetFormatType(src_view.format);
    const bool dst_is_color = dst_type == SurfaceType::ColorTexture;
    const bool src_is_color = src_type == SurfaceType::ColorTexture;
    if (dst_is_color == src_is_color) {
        // Same aspect, an ordinary copy already covers it.
        return;
    }

    const VideoCommon::Region2D region{
        .start = {0, 0},
        .end = {static_cast<s32>(pass.render_area_width),
                static_cast<s32>(pass.render_area_height)},
    };
    blit_image_helper.Convert(pass, src,
                              dst_is_color ? BlitImageHelper::ConvertKind::DepthToColor
                                           : BlitImageHelper::ConvertKind::ColorToDepth,
                              region, region, false);

    if (!dst_is_color && dst_view.format == PixelFormat::D24_UNORM_S8_UINT) {
        // We can't write the stencil half of a packed destination, Metal has no
        // stencil export from a fragment shader. Depth gets converted, stencil
        // keeps whatever it held.
        static std::atomic<u32> reported{0};
        if (reported++ < 4) {
            LOG_WARNING(Render_Metal,
                        "Converted depth into a packed depth-stencil target; stencil is kept");
        }
    }
}

namespace {

// One level of one layer as a 2D texture. Returns +1 like newTextureView does.
[[nodiscard]] Owned<MTL::Texture> SliceView(MTL::Texture* texture, MTL::PixelFormat format,
                                            u32 level, u32 layer) {
    return Adopt(texture->newTextureView(format, MTL::TextureType2D,
                                         NS::Range::Make(level, 1), NS::Range::Make(layer, 1)));
}

} // Anonymous namespace

void Image::BlitScale(MTL::Texture* src, MTL::Texture* dst) {
    if (src == nullptr || dst == nullptr) {
        return;
    }
    const bool is_color =
        VideoCore::Surface::GetFormatType(info.format) == SurfaceType::ColorTexture;
    const u32 levels = static_cast<u32>(std::min(src->mipmapLevelCount(), dst->mipmapLevelCount()));
    // A cube reports an arrayLength of one while holding six slices, so taking
    // arrayLength at face value copies one face and drops five.
    const auto slices_of = [](MTL::Texture* texture) {
        const MTL::TextureType type = texture->textureType();
        const bool is_cube =
            type == MTL::TextureTypeCube || type == MTL::TextureTypeCubeArray;
        return static_cast<u32>(texture->arrayLength()) * (is_cube ? 6u : 1u);
    };
    const u32 layers = std::min(slices_of(src), slices_of(dst));

    for (u32 level = 0; level < levels; ++level) {
        // From the textures instead of info, that way the two agree at every
        // level without us repeating the mip maths.
        const s32 src_w = static_cast<s32>(std::max<u64>(1, src->width() >> level));
        const s32 src_h = static_cast<s32>(std::max<u64>(1, src->height() >> level));
        const s32 dst_w = static_cast<s32>(std::max<u64>(1, dst->width() >> level));
        const s32 dst_h = static_cast<s32>(std::max<u64>(1, dst->height() >> level));
        const Region2D src_region{.start = {0, 0}, .end = {src_w, src_h}};
        const Region2D dst_region{.start = {0, 0}, .end = {dst_w, dst_h}};

        for (u32 layer = 0; layer < layers; ++layer) {
            Owned<MTL::Texture> src_view = SliceView(src, pixel_format, level, layer);
            Owned<MTL::Texture> dst_view = SliceView(dst, pixel_format, level, layer);
            if (!src_view || !dst_view) {
                continue;
            }
            // Sampled and rendered into respectively, so both have to be
            // registered. A view carries its own gpuResourceID and the parent's
            // residency doesn't cover it. KeepAlive below names them as
            // allocations too, so they leave the set when the submission retires.
            runtime->device.AddResidentAllocation(src_view.get());
            runtime->device.AddResidentAllocation(dst_view.get());
            if (is_color) {
                const auto filter = VideoCore::Surface::IsPixelFormatInteger(info.format)
                                        ? Tegra::Engines::Fermi2D::Filter::Point
                                        : Tegra::Engines::Fermi2D::Filter::Bilinear;
                runtime->blit_image_helper.BlitColor(dst_view.get(), src_view.get(), dst_region,
                                                     src_region, filter);
            } else {
                // Depth can't be a colour attachment so it goes through the
                // depth-writing conversion instead. Stencil rides along
                // untouched, Metal has no stencil export, so a rescaled packed
                // image keeps the stencil the destination already had.
                RenderPassState pass{};
                pass.depth_stencil.texture = dst_view.get();
                pass.depth_stencil.has_stencil =
                    VideoCore::Surface::GetFormatType(info.format) ==
                    SurfaceType::DepthStencil;
                pass.render_area_width = static_cast<u32>(dst_w);
                pass.render_area_height = static_cast<u32>(dst_h);
                pass.num_layers = 1;
                runtime->blit_image_helper.Convert(pass, src_view.get(),
                                                   BlitImageHelper::ConvertKind::DepthToDepth,
                                                   dst_region, src_region, false);
            }
            // The blits above are recorded, not executed, and a Metal 4 command
            // buffer takes no strong reference to what it touches, so these
            // views have to outlive the submission that reads them.
            runtime->scheduler.KeepAlive(src_view.get(), src_view.get());
            runtime->scheduler.KeepAlive(dst_view.get(), dst_view.get());
        }
    }
}

bool Image::ScaleUp(bool ignore) {
    if (runtime == nullptr || !runtime->resolution.active) {
        return false;
    }
    if (IsRescaled() || !texture) {
        return false;
    }
    if (info.type != VideoCommon::ImageType::e2D) {
        // A 3D image can't be sliced into the 2D views the blits need, Metal
        // won't view across texture classes, and render targets are 2D.
        return false;
    }
    if (samples > 1) {
        // The copy is a draw through a plain sampler, it can't resolve or spread
        // samples. Refusing keeps the whole MSAA framebuffer at 1x.
        return false;
    }
    if (!runtime->IsFormatScalable(info.format)) {
        // The copy is a draw, so the destination has to be renderable.
        return false;
    }

    if (!scaled_texture) {
        // Unexpand the sample count first, then rescale. The other way round
        // scales the sample expansion too.
        const auto [scale_samples_x, scale_samples_y] =
            VideoCommon::SamplesLog2(info.num_samples);
        const u32 base_width = info.size.width >> scale_samples_x;
        const u32 base_height = info.size.height >> scale_samples_y;
        const u32 width = runtime->resolution.ScaleUp(base_width);
        const u32 height = info.type == VideoCommon::ImageType::e2D
                               ? runtime->resolution.ScaleUp(base_height)
                               : base_height;

        ScopedAutoreleasePool pool;
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(ToTextureType(info));
        descriptor->setPixelFormat(pixel_format);
        descriptor->setWidth(width);
        descriptor->setHeight(height);
        descriptor->setDepth(info.type == VideoCommon::ImageType::e3D ? info.size.depth : 1);
        descriptor->setMipmapLevelCount(info.resources.levels);
        descriptor->setArrayLength(
            info.type == VideoCommon::ImageType::e3D ? 1 : info.resources.layers);
        descriptor->setSampleCount(samples);
        descriptor->setUsage(texture_usage);
        scaled_texture = runtime->memory_allocator.CreateTexture(descriptor, MemoryUsage::DeviceLocal);
        descriptor->release();

        if (!scaled_texture) {
            LOG_WARNING(Render_Metal, "Could not allocate a {}x{} scaled image", width, height);
            return false;
        }
        // Nothing to skip on a texture that has never held anything.
        ignore = false;
    }

    // Only set once the destination exists. Handle() switches on this flag, so
    // raising it before there's something to switch to hands out a null texture.
    flags |= VideoCommon::ImageFlagBits::Rescaled;
    has_scaled = true;
    if (!ignore) {
        BlitScale(texture.get(), scaled_texture.get());
    }
    runtime->BumpViewGeneration();
    return true;
}

bool Image::ScaleDown(bool ignore) {
    if (runtime == nullptr || !IsRescaled()) {
        return false;
    }
    flags &= ~VideoCommon::ImageFlagBits::Rescaled;
    if (!ignore && texture && scaled_texture) {
        BlitScale(scaled_texture.get(), texture.get());
    }
    runtime->BumpViewGeneration();
    return true;
}

void TextureCacheRuntime::BlitImage(Framebuffer* dst_framebuffer, ImageView& dst, ImageView& src,
                                    const Region2D& dst_region, const Region2D& src_region,
                                    Tegra::Engines::Fermi2D::Filter filter,
                                    Tegra::Engines::Fermi2D::Operation operation) {
    if (operation != Tegra::Engines::Fermi2D::Operation::SrcCopy &&
        operation != Tegra::Engines::Fermi2D::Operation::SrcCopyAnd) {
        // Blend and ROP operations need the destination read back into the
        // shader, which we don't do, the source is written straight over. Not a
        // bail-out on purpose, Vulkan does the same unblended copy and dropping
        // it loses the whole surface.
        static std::atomic<u32> reported{0};
        if (reported++ < 8) {
            LOG_WARNING(Render_Metal, "Blit operation {} is unblended; Metal copies the source",
                        static_cast<u32>(operation));
        }
    }

    // A depth destination can't be a colour attachment, so it takes the
    // depth-writing path instead of BlitColor. Stencil rides along untouched,
    // Metal has no stencil export.
    const auto surface_type = VideoCore::Surface::GetFormatType(dst.format);
    if (surface_type != SurfaceType::ColorTexture) {
        if (dst_framebuffer == nullptr) {
            return;
        }
        MTL::Texture* const depth_source = src.Handle(Shader::TextureType::Color2D);
        if (depth_source == nullptr) {
            return;
        }
        blit_image_helper.Convert(dst_framebuffer->GetRenderPassState(), depth_source,
                                  BlitImageHelper::ConvertKind::DepthToDepth, dst_region,
                                  src_region,
                                  filter == Tegra::Engines::Fermi2D::Filter::Bilinear);
        return;
    }

    // BlitColor binds texture2d<T>: use the 2D view (with SwizzleFix and single-slice
    // 2D type) for single-sample sources, keeping RenderTarget() for MSAA resolves.
    MTL::Texture* const source =
        src.Samples() > 1 ? src.RenderTarget()
                          : (src.Handle(Shader::TextureType::Color2D) != nullptr
                                 ? src.Handle(Shader::TextureType::Color2D)
                                 : src.RenderTarget());
    MTL::Texture* const destination = dst.RenderTarget();
    if (source == nullptr || destination == nullptr) {
        return;
    }

    // BlitColor's shaders declare texture2d<float> and its pipelines target a
    // single-sample attachment, so a multisampled texture on either side is a
    // type the shader can't sample at all.
    Owned<MTL::Texture> resolved_source;
    MTL::Texture* blit_source = source;
    if (src.Samples() > 1) {
        if (dst.Samples() > 1) {
            // Both multisampled. A resolve-then-expand round trip loses the
            // per-sample detail the destination exists to hold, so it's no
            // better than leaving the destination alone.
            static std::atomic<u32> reported{0};
            if (reported++ < 4) {
                LOG_WARNING(Render_Metal, "Skipping a {}x to {}x multisampled blit",
                            src.Samples(), dst.Samples());
            }
            return;
        }
        // Resolve to a single-sample copy and blit from that, same store-action
        // resolve CopyImageMSAA and the MSAA download path use.
        ScopedAutoreleasePool pool;
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(MTL::TextureType2D);
        descriptor->setPixelFormat(source->pixelFormat());
        descriptor->setWidth(source->width());
        descriptor->setHeight(source->height());
        descriptor->setMipmapLevelCount(1);
        descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        resolved_source = memory_allocator.CreateTexture(descriptor, MemoryUsage::DeviceLocal);
        descriptor->release();
        if (!resolved_source) {
            LOG_WARNING(Render_Metal, "No resolve target for a {}x multisampled blit",
                        src.Samples());
            return;
        }
        resolved_source->setLabel(MTLSTR("eden blit resolve"));

        const RenderPassState pass = BuildMsaaResolvePass(source, resolved_source.get());
        scheduler.RequestRenderPass(pass);
        scheduler.RequestOutsideRenderPassOperationContext();

        blit_source = resolved_source.get();
        // Named as an allocation too so the sweep drops it from the residency
        // set. Releasing it without that left the set pointing at freed memory,
        // and residency entries are wired, they can't be compressed or evicted,
        // so every stale one steals real physical memory.
        scheduler.KeepAlive(resolved_source.get(), resolved_source.get());
    } else if (dst.Samples() > 1) {
        // Single-sample source into a multisampled destination. That's what
        // ExpandToMultisample is for, but it writes the whole attachment from a
        // matching-size source and can't place a scaled region, which is what a
        // Fermi2D blit asks for. Refusing leaves the destination intact.
        static std::atomic<u32> reported{0};
        if (reported++ < 4) {
            LOG_WARNING(Render_Metal, "Skipping a blit into a {}x multisampled target",
                        dst.Samples());
        }
        return;
    }
    if (source->textureType() == MTL::TextureType3D ||
        destination->textureType() == MTL::TextureType3D) {
        // A volume render target can't go through the blit helper, its pipelines
        // sample texture2d.
        LOG_WARNING(Render_Metal, "Skipping 2D blit involving a 3D render target");
        return;
    }

    blit_image_helper.BlitColor(destination, blit_source, dst_region, src_region, filter);
}

u64 TextureCacheRuntime::AttachmentBarrierTick() const noexcept {
    return scheduler.RenderPassEndTick();
}

u64 TextureCacheRuntime::GetReadbackTick() const {
    return scheduler.CurrentTick();
}

bool TextureCacheRuntime::IsReadbackReady(u64 tick) const {
    return scheduler.IsFree(tick);
}

void TextureCacheRuntime::WaitReadback(u64 tick) {
    scheduler.Wait(tick);
}

void TextureCacheRuntime::SubmitReadbacks() {
    scheduler.Flush();
}

void TextureCacheRuntime::AccelerateImageUpload(
    Image&, const StagingBufferRef&, std::span<const VideoCommon::SwizzleParameters>) {
    LOG_WARNING(Render_Metal, "Accelerated image upload is not implemented");
}

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------

Image::Image(TextureCacheRuntime& runtime_, const ImageInfo& info_, GPUVAddr gpu_addr_,
             VAddr cpu_addr_)
    : VideoCommon::ImageBase(info_, gpu_addr_, cpu_addr_), runtime{&runtime_} {
    ScopedAutoreleasePool pool;

    const FormatInfo format_info = SurfaceFormat(runtime->device, info.format);
    if (format_info.support == FormatSupport::Unsupported) {
        LOG_ERROR(Render_Metal, "Unsupported image format {}", info.format);
        return;
    }

    // A compressed family this device doesn't have gets decoded on the CPU into
    // whatever format SurfaceFormat substituted. Without Converted the shared
    // cache uploads the still-compressed guest bytes into an RGBA8 texture.
    if (!IsCompressedFamilyNative(runtime->device, info.format)) {
        flags |= VideoCommon::ImageFlagBits::Converted;
        flags |= VideoCommon::ImageFlagBits::CostlyLoad;
    }
    // Same routing for a format that's just the wrong shape and not compressed.
    // Converted without CostlyLoad, the expansion is a linear pass and marking
    // it costly would skew eviction against cheap-to-rebuild images.
    if (VideoCommon::IsPixelFormatExpanded(info.format)) {
        flags |= VideoCommon::ImageFlagBits::Converted;
    }
    pixel_format = format_info.format;
    // Clamped, an unsupported count makes newTexture fail outright and leaves
    // the image and every view over it null, so a whole surface goes missing.
    samples = static_cast<u32>(info.num_samples);
    const u32 max_samples = runtime->device.GetMaxSampleCount();
    if (samples > max_samples) {
        LOG_WARNING(Render_Metal, "Clamping {}x MSAA to the device maximum of {}x", samples,
                    max_samples);
        samples = max_samples;
    }

    // The guest's extent counts samples as texels, Maxwell lays a multisampled
    // surface out spatially, so info.size is already sample-expanded. Making the
    // host texture at that size AND giving it a sample count allocates the
    // expansion twice and puts the rendered image in the top-left quadrant.
    const auto [samples_x, samples_y] = VideoCommon::SamplesLog2(info.num_samples);

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setTextureType(ToTextureType(info));
    descriptor->setPixelFormat(pixel_format);
    descriptor->setWidth(info.size.width >> samples_x);
    descriptor->setHeight(info.size.height >> samples_y);
    descriptor->setDepth(info.type == VideoCommon::ImageType::e3D ? info.size.depth : 1);
    // Metal requires a multisampled texture to have exactly one mip level.
    descriptor->setMipmapLevelCount(samples > 1 ? 1 : info.resources.levels);
    descriptor->setArrayLength(info.type == VideoCommon::ImageType::e3D
                                   ? 1
                                   : info.resources.layers);
    descriptor->setSampleCount(samples);
    texture_usage = UsageFor(info, format_info);
    descriptor->setUsage(texture_usage);

    texture = runtime->memory_allocator.CreateTexture(descriptor, MemoryUsage::DeviceLocal);
    descriptor->release();

    if (!texture) {
        LOG_ERROR(Render_Metal, "Failed to create a {}x{} image", info.size.width,
                  info.size.height);
        return;
    }
    texture->setLabel(MTLSTR("eden image"));

}

Image::Image(const VideoCommon::NullImageParams& params) : VideoCommon::ImageBase{params} {}

Image::~Image() {
    // Hand the textures to the scheduler instead of freeing them here. A Metal 4
    // command buffer holds no strong reference to what it touches, so an image
    // dropped while a recorded draw still samples it is a use-after-free. Passed
    // as allocations too so the sweep drops them from the residency set.
    if (runtime != nullptr) {
        runtime->ReleaseTirColorScratch(texture.get());
        runtime->ReleaseTirColorScratch(scaled_texture.get());
        runtime->scheduler.KeepAlive(texture.get(), texture.get());
        runtime->scheduler.KeepAlive(scaled_texture.get(), scaled_texture.get());
    }
}

namespace {

// Block metrics of the data that's actually in the staging buffer. Do NOT take
// these from info.format for a Converted image, the shared cache already
// rewrote the staging span into the host format while info.format still
// describes the guest's. Metal wants bytesPerRow and bytesPerImage in bytes, so
// getting it wrong shears every converted texture and misplaces every array
// layer past the first. The converted layout is 1x1 texels of
// ConvertedBytesPerBlock, the same oracle that sized the staging buffer, except
// for ASTC recompressed to BC which gives real 4x4 blocks.
struct StagingLayout {
    u32 bytes_per_block;
    u32 block_width;
    u32 block_height;
};

[[nodiscard]] StagingLayout StagingLayoutOf(const VideoCommon::ImageInfo& info,
                                            VideoCommon::ImageFlagBits flags) {
    using VideoCore::Surface::BytesPerBlock;
    using VideoCore::Surface::DefaultBlockHeight;
    using VideoCore::Surface::DefaultBlockWidth;

    if (False(flags & VideoCommon::ImageFlagBits::Converted)) {
        return {BytesPerBlock(info.format), DefaultBlockWidth(info.format),
                DefaultBlockHeight(info.format)};
    }

    const auto recompression = videoSettings.astc_recompression;
    if (VideoCore::Surface::IsPixelFormatASTC(info.format) &&
        recompression != AstcRecompression::Uncompressed) {
        // Recompressed instead of decoded, so real BC blocks come out.
        return {recompression == AstcRecompression::Bc1 ? 8u : 16u, 4u, 4u};
    }
    return {VideoCommon::ConvertedBytesPerBlock(info.format), 1u, 1u};
}

} // Anonymous namespace

void Image::UploadMemory(MTL::Buffer* buffer, u64 offset,
                         std::span<const VideoCommon::BufferImageCopy> copies) {
    UploadMemoryImpl(buffer, offset, copies, /*from_cpu_staging=*/false);
}

void Image::UploadMemoryImpl(MTL::Buffer* buffer, u64 offset,
                             std::span<const VideoCommon::BufferImageCopy> copies,
                             bool from_cpu_staging) {
    if (!texture || buffer == nullptr) {
        return;
    }
    // Uploads work on the UNSCALED texture, so a rescaled image has to come
    // down (ignoring scaled contents if overwritten) and scale back up after so
    // scaled_texture receives the new pixels.
    const bool was_rescaled = True(flags & VideoCommon::ImageFlagBits::Rescaled);
    if (was_rescaled) {
        ScaleDown(true);
    }
    SCOPE_EXIT {
        if (was_rescaled) {
            ScaleUp();
        }
    };
    // A freshly created, non-MSAA texture uploaded from CPU staging memory has no
    // prior readers or writers in this submission, so the copy can run on Metal 4's
    // upload command buffer without ending the active render pass (TBDR store/reload).
    const bool can_reorder = from_cpu_staging && !has_gpu_history && samples == 1 &&
                             !was_rescaled && attachment_write_tick == 0 && gpu_write_tick == 0 &&
                             !True(flags & VideoCommon::ImageFlagBits::GpuModified) &&
                             !videoSettings.disable_buffer_reorder;
    has_gpu_history = true;
    // A blit can't write a multisampled destination, so the upload lands in a
    // single-sample staging texture and a draw spreads it across the samples.
    // Allocated per upload, MSAA images are rarely re-uploaded.
    Owned<MTL::Texture> staging;
    if (samples > 1) {
        // Single-layer only. The staging texture is a plain 2D one but the copy
        // loop addresses base_layer + layer, and ExpandToMultisample draws over
        // the whole attachment with no slice of its own.
        if (info.resources.layers > 1) {
            static std::atomic<u32> reported{0};
            if (reported++ < 4) {
                LOG_WARNING(Render_Metal,
                            "Skipping the upload of a {}x MSAA image with {} layers; only slice 0 "
                            "could be written and the rest would be out of range",
                            samples, info.resources.layers);
            }
            return;
        }
        ScopedAutoreleasePool pool;
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(MTL::TextureType2D);
        descriptor->setPixelFormat(pixel_format);
        descriptor->setWidth(info.size.width);
        descriptor->setHeight(info.size.height);
        descriptor->setMipmapLevelCount(1);
        descriptor->setUsage(MTL::TextureUsageShaderRead);
        staging = runtime->memory_allocator.CreateTexture(descriptor, MemoryUsage::DeviceLocal);
        descriptor->release();
        if (!staging) {
            LOG_WARNING(Render_Metal, "No staging texture for a {}x MSAA upload", samples);
            return;
        }
        staging->setLabel(MTLSTR("eden MSAA upload staging"));
    }

    boost::container::small_vector<VideoCommon::BufferImageCopy, 8> owned(copies.begin(),
                                                                          copies.end());
    MTL::Texture* dst = staging ? staging.get() : texture.get();
    const auto [bytes_per_block, block_width, block_height] = StagingLayoutOf(info, flags);

    // The staging span is fully written by now. Publishing the whole region is
    // coarse but correct, and a no-op on unified memory.
    if (MemoryAllocator::HasManagedBuffers()) {
        MemoryAllocator::FlushRange(buffer, offset, buffer->length() - offset);
    }

    // A combined depth/stencil texture holds two interleaved aspects and Metal
    // refuses a copy that doesn't name one. The guest hands over the depth
    // aspect, and the two options are mutually exclusive so one copy can't carry
    // both.
    const MTL::BlitOption blit_options =
        IsCombinedDepthStencil(dst->pixelFormat()) ? MTL::BlitOptionDepthFromDepthStencil
                                                   : MTL::BlitOptionNone;

    // Depth/stencil guest layouts that cannot be blitted raw into Metal's
    // Depth32Float / Depth32Float_Stencil8 textures:
    //  - S8_UINT_D24_UNORM (Maxwell Z24S8): depth bits 8..31, stencil 0..7
    //  - D24_UNORM_S8_UINT (Maxwell S8Z24): depth bits 0..23, stencil 24..31
    //  - X8_D24_UNORM      (Maxwell X8Z24): depth bits 0..23 (no stencil)
    //  - D32_FLOAT_S8_UINT (Maxwell Z32F_X24S8): 8-byte [float32, u8 + 3B pad]
    enum class DepthUploadKind { None, S8D24, D24S8, X8D24, D32S8 };
    const DepthUploadKind depth_upload_kind = [&]() {
        if (buffer->contents() == nullptr) {
            return DepthUploadKind::None;
        }
        if (dst->pixelFormat() == MTL::PixelFormatDepth32Float_Stencil8) {
            if (info.format == PixelFormat::S8_UINT_D24_UNORM && bytes_per_block == 4) {
                return DepthUploadKind::S8D24;
            }
            if (info.format == PixelFormat::D24_UNORM_S8_UINT && bytes_per_block == 4) {
                return DepthUploadKind::D24S8;
            }
            if (info.format == PixelFormat::D32_FLOAT_S8_UINT && bytes_per_block == 8) {
                return DepthUploadKind::D32S8;
            }
        } else if (dst->pixelFormat() == MTL::PixelFormatDepth32Float &&
                   info.format == PixelFormat::X8_D24_UNORM && bytes_per_block == 4) {
            return DepthUploadKind::X8D24;
        }
        return DepthUploadKind::None;
    }();

    if (depth_upload_kind != DepthUploadKind::None) {
        const bool has_stencil_plane = depth_upload_kind != DepthUploadKind::X8D24;
        const u32 src_texel_bytes = depth_upload_kind == DepthUploadKind::D32S8 ? 8u : 4u;
        struct PlaneSpan {
            u64 depth_offset;
            u64 stencil_offset;
            u32 row_texels;
            u32 image_texels;
        };
        boost::container::small_vector<PlaneSpan, 8> spans;
        u64 scratch_bytes = 0;
        const auto align16 = [](u64 value) { return (value + 15u) & ~u64{15}; };
        for (const VideoCommon::BufferImageCopy& copy : owned) {
            const u32 row_texels =
                copy.buffer_row_length != 0 ? copy.buffer_row_length : copy.image_extent.width;
            const u32 image_texels =
                copy.buffer_image_height != 0 ? copy.buffer_image_height : copy.image_extent.height;
            const u64 texels = u64{row_texels} * image_texels;
            for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
                const u64 depth_offset = align16(scratch_bytes);
                const u64 stencil_offset =
                    has_stencil_plane ? align16(depth_offset + texels * 4) : 0;
                scratch_bytes = has_stencil_plane ? (stencil_offset + texels)
                                                  : (depth_offset + texels * 4);
                spans.push_back(PlaneSpan{depth_offset, stencil_offset, row_texels, image_texels});
            }
        }

        const StagingBufferRef scratch =
            runtime->staging_buffer_pool.Request(static_cast<size_t>(scratch_bytes),
                                                 MemoryUsage::Upload);
        if (scratch.buffer == nullptr || scratch.mapped_span.empty()) {
            LOG_WARNING(Render_Metal, "No staging space to split a {} byte depth/stencil upload",
                        scratch_bytes);
            if (staging) {
                runtime->device.RemoveResidentAllocation(staging.get());
                runtime->device.CommitResidency();
            }
            return;
        }

        const u8* const src_base = static_cast<const u8*>(buffer->contents()) + offset;
        u8* const dst_base = scratch.mapped_span.data();
        size_t span_index = 0;
        for (const VideoCommon::BufferImageCopy& copy : owned) {
            const PlaneSpan& first = spans[span_index];
            const u64 src_bytes_per_image =
                u64{first.row_texels} * src_texel_bytes * first.image_texels;
            for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
                const PlaneSpan& span = spans[span_index++];
                const u8* src = src_base + copy.buffer_offset + layer * src_bytes_per_image;
                u8* depth_plane = dst_base + span.depth_offset;
                u8* stencil_plane = has_stencil_plane ? dst_base + span.stencil_offset : nullptr;
                const u64 texel_count = u64{span.image_texels} * span.row_texels;
                auto* const depth_f32 = reinterpret_cast<float*>(depth_plane);
                constexpr float INV_D24 = 1.0f / 16777215.0f;
                switch (depth_upload_kind) {
                case DepthUploadKind::D32S8: {
                    const auto* const src32 = reinterpret_cast<const u32*>(src);
                    auto* const depth_u32 = reinterpret_cast<u32*>(depth_plane);
                    for (u64 idx = 0; idx < texel_count; ++idx) {
                        depth_u32[idx] = src32[idx * 2];
                        stencil_plane[idx] = static_cast<u8>(src32[idx * 2 + 1] & 0xFFu);
                    }
                    break;
                }
                case DepthUploadKind::S8D24: {
                    const auto* const src32 = reinterpret_cast<const u32*>(src);
                    for (u64 idx = 0; idx < texel_count; ++idx) {
                        const u32 packed = src32[idx];
                        depth_f32[idx] = static_cast<float>(packed >> 8) * INV_D24;
                        stencil_plane[idx] = static_cast<u8>(packed & 0xFFu);
                    }
                    break;
                }
                case DepthUploadKind::D24S8: {
                    const auto* const src32 = reinterpret_cast<const u32*>(src);
                    for (u64 idx = 0; idx < texel_count; ++idx) {
                        const u32 packed = src32[idx];
                        depth_f32[idx] = static_cast<float>(packed & 0xFFFFFFu) * INV_D24;
                        stencil_plane[idx] = static_cast<u8>(packed >> 24);
                    }
                    break;
                }
                case DepthUploadKind::X8D24: {
                    const auto* const src32 = reinterpret_cast<const u32*>(src);
                    for (u64 idx = 0; idx < texel_count; ++idx) {
                        depth_f32[idx] = static_cast<float>(src32[idx] & 0xFFFFFFu) * INV_D24;
                    }
                    break;
                }
                case DepthUploadKind::None:
                    break;
                }
            }
        }
        MemoryAllocator::FlushRange(scratch.buffer, scratch.offset,
                                    static_cast<u64>(scratch_bytes));

        const MTL::BlitOption depth_option =
            has_stencil_plane ? MTL::BlitOptionDepthFromDepthStencil : MTL::BlitOptionNone;
        const auto encode_depth_copies = [](auto* encoder, MTL::Texture* target,
                                            const StagingBufferRef& src_ref, bool stencil_plane,
                                            MTL::BlitOption d_opt,
                                            const boost::container::small_vector<PlaneSpan, 8>& sp,
                                            const boost::container::small_vector<
                                                VideoCommon::BufferImageCopy, 8>& cps) {
            size_t span_index = 0;
            for (const VideoCommon::BufferImageCopy& copy : cps) {
                for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
                    const PlaneSpan& span = sp[span_index++];
                    const MTL::Size extent =
                        MTL::Size::Make(copy.image_extent.width, copy.image_extent.height,
                                        copy.image_extent.depth);
                    const u64 texels = u64{span.row_texels} * span.image_texels;
                    encoder->copyFromBuffer(src_ref.buffer, src_ref.offset + span.depth_offset,
                                            span.row_texels * 4u, texels * 4u, extent, target,
                                            copy.image_subresource.base_layer + layer,
                                            copy.image_subresource.base_level,
                                            ToOrigin(copy.image_offset), d_opt);
                    if (stencil_plane) {
                        encoder->copyFromBuffer(src_ref.buffer,
                                                src_ref.offset + span.stencil_offset,
                                                span.row_texels, texels, extent, target,
                                                copy.image_subresource.base_layer + layer,
                                                copy.image_subresource.base_level,
                                                ToOrigin(copy.image_offset),
                                                MTL::BlitOptionStencilFromDepthStencil);
                    }
                }
            }
        };
        if (can_reorder) {
            runtime->scheduler.RecordUpload(
                [dst, scratch, has_stencil_plane, depth_option, encode_depth_copies,
                 spans = std::move(spans), copies = std::move(owned)](
                    Scheduler&, MTL4::ComputeCommandEncoder* encoder) {
                    encode_depth_copies(encoder, dst, scratch, has_stencil_plane, depth_option,
                                        spans, copies);
                });
        } else {
            runtime->scheduler.RequestOutsideRenderPassOperationContext();
            runtime->scheduler.Record([dst, scratch, has_stencil_plane, depth_option,
                                       encode_depth_copies, spans = std::move(spans),
                                       copies = std::move(owned)](CommandContext& context) {
                BlitEncoder encoder = context.RequestBlit();
                if (encoder == nullptr) {
                    return;
                }
                encode_depth_copies(encoder.operator->(), dst, scratch, has_stencil_plane,
                                    depth_option, spans, copies);
            });
        }

        if (staging) {
            runtime->blit_image_helper.ExpandToMultisample(texture.get(), staging.get());
            // Named as an allocation too so the sweep drops it from the
            // residency set, those entries are wired and can't be evicted.
            runtime->scheduler.KeepAlive(staging.get(), staging.get());
        }
        return;
    }

    const auto encode_copies = [](auto* encoder, MTL::Texture* target, MTL::Buffer* src_buf,
                                  u64 src_off, u32 bpb, u32 bw, u32 bh, MTL::BlitOption opts,
                                  const boost::container::small_vector<
                                      VideoCommon::BufferImageCopy, 8>& cps) {
        for (const VideoCommon::BufferImageCopy& copy : cps) {
            // Metal wants bytes, not texels, and whole blocks for compressed
            // formats. The row length MUST come from buffer_row_length, which
            // describes the source buffer. Taking the stride from image_extent
            // shears the upload into diagonal bands.
            const u32 row_texels =
                copy.buffer_row_length != 0 ? copy.buffer_row_length : copy.image_extent.width;
            const u32 image_texels =
                copy.buffer_image_height != 0 ? copy.buffer_image_height : copy.image_extent.height;
            const u32 blocks_across = (row_texels + bw - 1) / bw;
            const u32 blocks_down = (image_texels + bh - 1) / bh;
            const NS::UInteger bytes_per_row = blocks_across * bpb;
            const NS::UInteger bytes_per_image = bytes_per_row * blocks_down;

            for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
                encoder->copyFromBuffer(
                    src_buf, src_off + copy.buffer_offset + layer * bytes_per_image, bytes_per_row,
                    bytes_per_image,
                    MTL::Size::Make(copy.image_extent.width, copy.image_extent.height,
                                    copy.image_extent.depth),
                    target, copy.image_subresource.base_layer + layer,
                    copy.image_subresource.base_level, ToOrigin(copy.image_offset), opts);
            }
        }
    };

    if (can_reorder) {
        runtime->scheduler.RecordUpload(
            [dst, buffer, offset, bytes_per_block, block_width, block_height, blit_options,
             encode_copies, copies = std::move(owned)](Scheduler&,
                                                       MTL4::ComputeCommandEncoder* encoder) {
                encode_copies(encoder, dst, buffer, offset, bytes_per_block, block_width,
                              block_height, blit_options, copies);
            });
    } else {
        runtime->scheduler.RequestOutsideRenderPassOperationContext();
        runtime->scheduler.Record([dst, buffer, offset, bytes_per_block, block_width, block_height,
                                   blit_options, encode_copies,
                                   copies = std::move(owned)](CommandContext& context) {
            BlitEncoder encoder = context.RequestBlit();
            if (encoder == nullptr) {
                return;
            }
            encode_copies(encoder.operator->(), dst, buffer, offset, bytes_per_block, block_width,
                          block_height, blit_options, copies);
        });
    }

    if (staging) {
        // Now spread it across every sample. The staging texture goes to a
        // recorded command so it outlives the expand instead of dying here.
        runtime->blit_image_helper.ExpandToMultisample(texture.get(), staging.get());
        // Named as an allocation too so the sweep drops it from the residency
        // set, those entries are wired and can't be evicted.
        runtime->scheduler.KeepAlive(staging.get(), staging.get());
    }
}

void Image::UploadMemory(const StagingBufferRef& map,
                         std::span<const VideoCommon::BufferImageCopy> copies) {
    UploadMemoryImpl(map.buffer, map.offset, copies, /*from_cpu_staging=*/true);
}

void Image::DownloadMemory(MTL::Buffer* buffer, size_t offset,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    has_gpu_history = true;
    if (!texture || buffer == nullptr) {
        return;
    }
    // Uploads and downloads work on the UNSCALED texture, so a rescaled image
    // has to come down first and go back up after. Handle() returns
    // scaled_texture while the Rescaled flag is set.
    const bool was_rescaled = True(flags & VideoCommon::ImageFlagBits::Rescaled);
    if (was_rescaled) {
        ScaleDown(false);
    }
    SCOPE_EXIT {
        if (was_rescaled) {
            ScaleUp(true);
        }
    };

    boost::container::small_vector<VideoCommon::BufferImageCopy, 8> owned(copies.begin(),
                                                                          copies.end());
    MTL::Texture* src = texture.get();

    // A blit encoder can't read a multisampled texture, so resolve first through
    // a pass whose store action is Resolve and blit from that. It's lossy, the
    // guest gets the sample average, but it's the only readback Metal gives us.
    Owned<MTL::Texture> resolved;
    if (info.num_samples > 1) {
        const bool is_color = VideoCore::Surface::GetFormatType(info.format) ==
                              VideoCore::Surface::SurfaceType::ColorTexture;
        // A resolve store action carries colour only, RenderPassState has no
        // resolve texture for the depth attachment. Refusing gives back stale
        // contents, which beats an undefined blit.
        if (!is_color || info.resources.layers > 1) {
            static std::atomic<u32> reported{0};
            if (reported++ < 4) {
                LOG_WARNING(Render_Metal,
                            "Cannot read back a {}x MSAA image with {} layer(s), format {}: no "
                            "resolve path for it",
                            info.num_samples, info.resources.layers,
                            static_cast<u32>(info.format));
            }
            return;
        }

        ScopedAutoreleasePool pool;
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(MTL::TextureType2D);
        descriptor->setPixelFormat(texture->pixelFormat());
        descriptor->setWidth(texture->width());
        descriptor->setHeight(texture->height());
        descriptor->setMipmapLevelCount(1);
        descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
        resolved = runtime->memory_allocator.CreateTexture(descriptor, MemoryUsage::DeviceLocal);
        descriptor->release();
        if (!resolved) {
            LOG_WARNING(Render_Metal, "No resolve target for a {}x MSAA download",
                        info.num_samples);
            return;
        }
        resolved->setLabel(MTLSTR("eden MSAA download resolve"));
        // Keep the transient resolve target alive and resident until the GPU
        // finishes both the resolve pass and the blit that reads from it.
        runtime->scheduler.KeepAlive(resolved.get(), resolved.get());

        const RenderPassState pass = BuildMsaaResolvePass(texture.get(), resolved.get());
        runtime->scheduler.RequestRenderPass(pass);
        // No draw, the resolve is the store action so ending the pass is the
        // whole operation.
        runtime->scheduler.RequestOutsideRenderPassOperationContext();

        src = resolved.get();
    }

    const auto [bytes_per_block, block_width, block_height] = StagingLayoutOf(info, flags);

    // See Image::UploadMemory, reading out of a combined depth/stencil texture
    // has to name the aspect too.
    const MTL::BlitOption blit_options =
        IsCombinedDepthStencil(src->pixelFormat()) ? MTL::BlitOptionDepthFromDepthStencil
                                                   : MTL::BlitOptionNone;

    // Re-interleave separate depth & stencil planes into the guest's depth/stencil layout.
    std::optional<BlitImageHelper::DepthPackLayout> pack_layout;
    if (src->pixelFormat() == MTL::PixelFormatDepth32Float_Stencil8) {
        if (info.format == PixelFormat::S8_UINT_D24_UNORM && bytes_per_block == 4) {
            pack_layout = BlitImageHelper::DepthPackLayout::S8D24;
        } else if (info.format == PixelFormat::D24_UNORM_S8_UINT && bytes_per_block == 4) {
            pack_layout = BlitImageHelper::DepthPackLayout::D24S8;
        } else if (info.format == PixelFormat::D32_FLOAT_S8_UINT && bytes_per_block == 8) {
            pack_layout = BlitImageHelper::DepthPackLayout::D32S8;
        }
    } else if (src->pixelFormat() == MTL::PixelFormatDepth32Float &&
               info.format == PixelFormat::X8_D24_UNORM && bytes_per_block == 4) {
        pack_layout = BlitImageHelper::DepthPackLayout::X8D24;
    }

    if (pack_layout) {
        const bool has_stencil_plane = *pack_layout != BlitImageHelper::DepthPackLayout::X8D24;
        const u32 dst_texel_bytes =
            *pack_layout == BlitImageHelper::DepthPackLayout::D32S8 ? 8u : 4u;
        struct PlaneSpan {
            u64 depth_offset;
            u64 stencil_offset;
            u32 row_texels;
            u32 image_texels;
        };
        boost::container::small_vector<PlaneSpan, 8> spans;
        u64 scratch_bytes = 0;
        const auto align16 = [](u64 value) { return (value + 15u) & ~u64{15}; };
        for (const VideoCommon::BufferImageCopy& copy : owned) {
            const u32 row_texels =
                copy.buffer_row_length != 0 ? copy.buffer_row_length : copy.image_extent.width;
            const u32 image_texels =
                copy.buffer_image_height != 0 ? copy.buffer_image_height : copy.image_extent.height;
            const u64 texels = u64{row_texels} * image_texels;
            for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
                const u64 depth_offset = align16(scratch_bytes);
                const u64 stencil_offset =
                    has_stencil_plane ? align16(depth_offset + texels * 4) : depth_offset;
                scratch_bytes = has_stencil_plane ? (stencil_offset + texels)
                                                  : (depth_offset + texels * 4);
                spans.push_back(PlaneSpan{depth_offset, stencil_offset, row_texels, image_texels});
            }
        }

        // DeviceLocal, the GPU writes these planes and reads them back in the
        // kernel. Nothing on the CPU ever looks at them.
        const StagingBufferRef scratch = runtime->staging_buffer_pool.Request(
            static_cast<size_t>(scratch_bytes), MemoryUsage::DeviceLocal);
        if (scratch.buffer == nullptr) {
            LOG_WARNING(Render_Metal, "No staging space to split a {} byte depth/stencil download",
                        scratch_bytes);
            return;
        }

        const MTL::BlitOption depth_option =
            has_stencil_plane ? MTL::BlitOptionDepthFromDepthStencil : MTL::BlitOptionNone;
        runtime->scheduler.RequestOutsideRenderPassOperationContext();
        {
            auto span_copy = spans;
            runtime->scheduler.Record([src, scratch, has_stencil_plane, depth_option,
                                       spans = std::move(span_copy),
                                       copies = owned](CommandContext& context) {
                BlitEncoder encoder = context.RequestBlit();
                if (encoder == nullptr) {
                    return;
                }
                size_t span_index = 0;
                for (const VideoCommon::BufferImageCopy& copy : copies) {
                    for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
                        const PlaneSpan& span = spans[span_index++];
                        const MTL::Size extent =
                            MTL::Size::Make(copy.image_extent.width, copy.image_extent.height,
                                            copy.image_extent.depth);
                        const u64 texels = u64{span.row_texels} * span.image_texels;
                        encoder->copyFromTexture(
                            src, copy.image_subresource.base_layer + layer,
                            copy.image_subresource.base_level, ToOrigin(copy.image_offset), extent,
                            scratch.buffer, scratch.offset + span.depth_offset,
                            span.row_texels * 4u, texels * 4u, depth_option);
                        if (has_stencil_plane) {
                            encoder->copyFromTexture(
                                src, copy.image_subresource.base_layer + layer,
                                copy.image_subresource.base_level, ToOrigin(copy.image_offset),
                                extent, scratch.buffer, scratch.offset + span.stencil_offset,
                                span.row_texels, texels, MTL::BlitOptionStencilFromDepthStencil);
                        }
                    }
                }
            });
        }

        // Then fold the planes back together, one dispatch per region. The
        // encoder switch from blit to compute is what orders this after them.
        size_t span_index = 0;
        for (const VideoCommon::BufferImageCopy& copy : owned) {
            for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
                const PlaneSpan& span = spans[span_index++];
                runtime->blit_image_helper.PackDepthStencil(
                    buffer,
                    offset + copy.buffer_offset +
                        static_cast<u64>(layer) * span.row_texels * dst_texel_bytes *
                            span.image_texels,
                    span.row_texels, scratch.buffer, scratch.offset + span.depth_offset,
                    scratch.offset + span.stencil_offset, span.row_texels,
                    copy.image_extent.width, copy.image_extent.height, *pack_layout);
            }
        }
        return;
    }

    // Ends the pass on the recording side too. RequestBlit closes the render
    // encoder on the worker, but without this the recording thread's mirror
    // still thinks a pass is open and every later check of that is wrong.
    runtime->scheduler.RequestOutsideRenderPassOperationContext();
    runtime->scheduler.Record([src, buffer, offset, bytes_per_block, block_width, block_height,
                               blit_options, copies = std::move(owned)](CommandContext& context) {
        BlitEncoder encoder = context.RequestBlit();
        if (encoder == nullptr) {
            return;
        }
        for (const VideoCommon::BufferImageCopy& copy : copies) {
            // Same as UploadMemory, the buffer's layout comes from
            // buffer_row_length/buffer_image_height and not from the region.
            const u32 row_texels =
                copy.buffer_row_length != 0 ? copy.buffer_row_length : copy.image_extent.width;
            const u32 image_texels =
                copy.buffer_image_height != 0 ? copy.buffer_image_height : copy.image_extent.height;
            const u32 blocks_across = (row_texels + block_width - 1) / block_width;
            const u32 blocks_down = (image_texels + block_height - 1) / block_height;
            const NS::UInteger bytes_per_row = blocks_across * bytes_per_block;
            const NS::UInteger bytes_per_image = bytes_per_row * blocks_down;

            for (s32 layer = 0; layer < copy.image_subresource.num_layers; ++layer) {
                encoder->copyFromTexture(
                    src, copy.image_subresource.base_layer + layer,
                    copy.image_subresource.base_level, ToOrigin(copy.image_offset),
                    MTL::Size::Make(copy.image_extent.width, copy.image_extent.height,
                                    copy.image_extent.depth),
                    buffer, offset + copy.buffer_offset + layer * bytes_per_image, bytes_per_row,
                    bytes_per_image, blit_options);
            }
        }
    });
}

void Image::DownloadMemory(std::span<MTL::Buffer*> buffers, std::span<size_t> offsets,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    for (size_t i = 0; i < buffers.size(); ++i) {
        DownloadMemory(buffers[i], offsets[i], copies);
    }
}

void Image::DownloadMemory(const StagingBufferRef& map,
                           std::span<const VideoCommon::BufferImageCopy> copies) {
    DownloadMemory(map.buffer, map.offset, copies);
}

// ---------------------------------------------------------------------------
// ImageView
// ---------------------------------------------------------------------------

ImageView::ImageView(TextureCacheRuntime& runtime_, const VideoCommon::ImageViewInfo& info_,
                     ImageId image_id_, Image& image)
    : VideoCommon::ImageViewBase{info_, image.info, image_id_, image.gpu_addr}, runtime{&runtime_},
      image_handle{image.Handle()},
      is_integer_format{VideoCore::Surface::IsPixelFormatInteger(info_.format)},
      pixel_format{image.Format()}, samples{image.Samples()} {
    runtime->BumpViewGeneration();
    if (image_handle == nullptr) {
        return;
    }

    // The format every view of this image uses. The attachment view and the
    // sampled views MUST agree or the guest renders through one interpretation
    // and samples back through another.
    const FormatInfo view_format = SurfaceFormat(runtime->device, info_.format);
    const MTL::PixelFormat sample_format =
        ViewSampleFormat(view_format, info_.format, image.info.format, pixel_format);

    // Views are NOT added to the device residency set on purpose. A view owns no
    // memory, it aliases the parent's pages, and the parent went in when
    // MemoryAllocator created it. Validation has never asked for one.
    //
    // newTextureView returns a +1 object so adopt it, don't retain. The
    // attachment view keeps the image's texture type. The shader-visible view
    // uses the type the guest declared and is the only one carrying the swizzle,
    // Metal refuses to render into a swizzled view.
    if (image_handle->textureType() == MTL::TextureType3D) {
        // newTextureView can't narrow a 3D texture to one depth plane, so the
        // attachment is the whole volume and we pick the subresource at
        // pass-build time with the attachment's level + depthPlane.
        render_target = Adopt(image_handle->newTextureView(sample_format));
        // This skips MakeView so it needs the same registration, an attachment
        // is addressed by its own gpuResourceID like any other view.
        if (render_target && runtime != nullptr) {
            runtime->device.AddResidentAllocation(render_target.get());
        }
        rt_level = range.base.level;
        rt_slice = range.base.layer;
        const u32 mip_depth =
            std::max<u32>(1, static_cast<u32>(image_handle->depth()) >> rt_level);
        const u32 available = mip_depth > rt_slice ? mip_depth - rt_slice : 1;
        // Metal forbids a non-zero depthPlane when renderTargetArrayLength > 1, so a 3D
        // attachment targeting a non-zero slice (or a single-slice view, e.g. slice-by-slice
        // 3D LUT rendering in Xenoblade 3) must keep rt_layers == 1 so the render pass sets
        // depthPlane = rt_slice instead of overwriting depthPlane 0.
        rt_layers = (rt_slice > 0 || range.extent.layers <= 1)
                        ? 1u
                        : std::clamp(static_cast<u32>(range.extent.layers), 1u, available);
    } else {
        render_target = Adopt(MakeView(sample_format, image_handle->textureType(),
                                       range.base.level, range.extent.levels, range.base.layer,
                                       range.extent.layers, nullptr));
    }

    const MTL::TextureSwizzleChannels swizzle = MakeSwizzle(info_, view_format.swizzle_fix);
    const bool identity = IsIdentitySwizzle(swizzle);

    // Which ASPECT of a packed depth/stencil image the guest wants to sample.
    // Maxwell names the components positionally and the order depends on the
    // format, so a swizzle mentioning R means depth for D24S8/D32S8 and stencil
    // for S8_D24. Metal spells the stencil aspect as the X<n>_Stencil8 companion
    // format, and the attachment view keeps the depth format.
    MTL::PixelFormat sampled_format_value = sample_format;
    if (pixel_format == MTL::PixelFormatDepth32Float_Stencil8 ||
        pixel_format == MTL::PixelFormatDepth24Unorm_Stencil8) {
        const auto sources = info_.Swizzle();
        const bool any_red = std::ranges::any_of(
            sources, [](Tegra::Texture::SwizzleSource c) {
                return c == Tegra::Texture::SwizzleSource::R;
            });
        const bool red_is_stencil = info_.format == PixelFormat::S8_UINT_D24_UNORM;
        const bool wants_stencil = any_red == red_is_stencil;
        if (wants_stencil && !info_.IsRenderTarget()) {
            sampled_format_value = pixel_format == MTL::PixelFormatDepth32Float_Stencil8
                                 ? MTL::PixelFormatX32_Stencil8
                                 : MTL::PixelFormatX24_Stencil8;
        }
    }

    // We build both the single-layer and the array view of each class, the
    // shader decides how it samples and doesn't have to match the guest's
    // declared view type. Kept so Handle() can build others on demand.
    sampled_format = sampled_format_value;
    view_swizzle = swizzle;
    view_swizzle_identity = identity;
    reads_like_identity = ReadsLikeIdentity(swizzle, sampled_format_value);

    const auto create = [&](Shader::TextureType shader_type, u32 num_layers) {
        views[static_cast<size_t>(shader_type)] =
            Adopt(MakeView(sampled_format_value, ToTextureType(shader_type), range.base.level,
                           range.extent.levels, range.base.layer, num_layers,
                           identity ? nullptr : &swizzle));
    };

    switch (type) {
    case VideoCommon::ImageViewType::e1D:
    case VideoCommon::ImageViewType::e1DArray:
        create(Shader::TextureType::Color1D, 1);
        create(Shader::TextureType::ColorArray1D, range.extent.layers);
        break;
    case VideoCommon::ImageViewType::e2D:
    case VideoCommon::ImageViewType::e2DArray:
    case VideoCommon::ImageViewType::Rect:
        // Never eagerly resolve a 2DMultisample render target during view creation:
        // most MSAA targets are never sampled as 2D, and Handle() builds the resolved
        // stand-in lazily on first shader use.
        if (image_handle->textureType() != MTL::TextureType2DMultisample) {
            create(Shader::TextureType::Color2D, 1);
            create(Shader::TextureType::ColorArray2D, range.extent.layers);
        }
        break;
    case VideoCommon::ImageViewType::e3D:
        create(Shader::TextureType::Color3D, range.extent.layers);
        break;
    case VideoCommon::ImageViewType::Cube:
    case VideoCommon::ImageViewType::CubeArray:
        create(Shader::TextureType::ColorCube, 6);
        create(Shader::TextureType::ColorArrayCube, range.extent.layers);
        break;
    case VideoCommon::ImageViewType::Buffer:
        create(Shader::TextureType::Buffer, range.extent.layers);
        break;
    }
}

ImageView::ImageView(TextureCacheRuntime& runtime_, const VideoCommon::ImageViewInfo& info_,
                     ImageId image_id_, Image& image, const Common::SlotVector<Image>& slot_images_)
    : ImageView{runtime_, info_, image_id_, image} {
    slot_images = &slot_images_;
    if (volume_copy_num_levels != 0) {
        volume_copy_mod_tick = image.modification_tick;
        volume_copy_write_tick = image.attachment_write_tick;
    }
}

ImageView::ImageView(TextureCacheRuntime& runtime_, const VideoCommon::ImageInfo& info_,
                     const VideoCommon::ImageViewInfo& view_info, GPUVAddr gpu_addr_)
    : VideoCommon::ImageViewBase{info_, view_info, gpu_addr_}, runtime{&runtime_},
      buffer_size{VideoCommon::CalculateGuestSizeInBytes(info_)} {}

ImageView::ImageView(TextureCacheRuntime& runtime_,
                     const VideoCommon::NullImageViewParams& params)
    : VideoCommon::ImageViewBase{params}, runtime{&runtime_} {
    // Metal 4 Tier 2 argument buffers have no hardware nullDescriptor: sampling a
    // zero gpuResourceID faults/hangs the GPU. Back NULL_IMAGE_VIEW_ID with real
    // 1x1 zero-filled resident textures for every shader texture dimension,
    // matching Vulkan's NullImage fallback when VK_EXT_robustness2 is unavailable.
    if (runtime == nullptr || runtime->device.GetDevice() == nullptr) {
        return;
    }
    ScopedAutoreleasePool pool;
    const MTL::StorageMode storage_mode = runtime->device.HasUnifiedMemory()
                                              ? MTL::StorageModeShared
                                              : MTL::StorageModeManaged;
    const MTL::TextureUsage usage = MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite |
                                    MTL::TextureUsagePixelFormatView;
    const u32 zero_texel = 0;

    auto make_backing = [&](MTL::TextureType tex_type, u32 layers, u32 depth,
                            NS::String* label) -> Owned<MTL::Texture> {
        MTL::TextureDescriptor* desc = MTL::TextureDescriptor::alloc()->init();
        desc->setTextureType(tex_type);
        desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        desc->setWidth(1);
        desc->setHeight(1);
        desc->setDepth(depth);
        desc->setMipmapLevelCount(1);
        desc->setArrayLength(layers);
        desc->setUsage(usage);
        desc->setStorageMode(storage_mode);
        Owned<MTL::Texture> tex = Adopt(runtime->device.GetDevice()->newTexture(desc));
        desc->release();
        if (tex) {
            tex->setLabel(label);
            if (tex_type == MTL::TextureType3D) {
                tex->replaceRegion(MTL::Region::Make3D(0, 0, 0, 1, 1, 1), 0, 0, &zero_texel,
                                   sizeof(u32), sizeof(u32));
            } else {
                for (u32 slice = 0; slice < layers; ++slice) {
                    tex->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, slice, &zero_texel,
                                       sizeof(u32), sizeof(u32));
                }
            }
            runtime->device.AddResidentAllocation(tex.get());
        }
        return tex;
    };

    null_backing_textures[0] =
        make_backing(MTL::TextureType2DArray, 6, 1, MTLSTR("nxemu null 2d/cube"));
    null_backing_textures[1] = make_backing(MTL::TextureType3D, 1, 1, MTLSTR("nxemu null 3d"));

    auto make_null_view = [&](Shader::TextureType shader_type, MTL::Texture* base,
                              MTL::TextureType view_type, u32 layers) {
        if (base == nullptr) {
            return;
        }
        Owned<MTL::Texture> view = Adopt(base->newTextureView(
            MTL::PixelFormatRGBA8Unorm, view_type, NS::Range::Make(0, 1),
            NS::Range::Make(0, layers)));
        if (view) {
            runtime->device.AddResidentAllocation(view.get());
            views[static_cast<size_t>(shader_type)] = std::move(view);
        }
    };

    make_null_view(Shader::TextureType::Color1D, null_backing_textures[0].get(),
                   MTL::TextureType2D, 1);
    make_null_view(Shader::TextureType::ColorArray1D, null_backing_textures[0].get(),
                   MTL::TextureType2DArray, 1);
    make_null_view(Shader::TextureType::Color2D, null_backing_textures[0].get(),
                   MTL::TextureType2D, 1);
    make_null_view(Shader::TextureType::ColorArray2D, null_backing_textures[0].get(),
                   MTL::TextureType2DArray, 1);
    make_null_view(Shader::TextureType::Color2DRect, null_backing_textures[0].get(),
                   MTL::TextureType2D, 1);
    make_null_view(Shader::TextureType::ColorCube, null_backing_textures[0].get(),
                   MTL::TextureTypeCube, 6);
    make_null_view(Shader::TextureType::ColorArrayCube, null_backing_textures[0].get(),
                   MTL::TextureTypeCubeArray, 6);
    make_null_view(Shader::TextureType::Color3D, null_backing_textures[1].get(),
                   MTL::TextureType3D, 1);

    // 1x1 4x MSAA zero-initialized backing so unbound texture2d_ms / texture2d_ms_array
    // descriptors never write GPU resource ID 0 into a Metal 4 argument buffer.
    {
        MTL::TextureDescriptor* ms_desc = MTL::TextureDescriptor::alloc()->init();
        ms_desc->setTextureType(MTL::TextureType2DMultisampleArray);
        ms_desc->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
        ms_desc->setWidth(1);
        ms_desc->setHeight(1);
        ms_desc->setMipmapLevelCount(1);
        ms_desc->setArrayLength(1);
        ms_desc->setSampleCount(4);
        ms_desc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView);
        ms_desc->setStorageMode(MTL::StorageModePrivate);
        Owned<MTL::Texture> ms_backing = Adopt(runtime->device.GetDevice()->newTexture(ms_desc));
        if (!ms_backing) {
            ms_desc->setTextureType(MTL::TextureType2DMultisample);
            ms_backing = Adopt(runtime->device.GetDevice()->newTexture(ms_desc));
        }
        ms_desc->release();
        if (ms_backing) {
            ms_backing->setLabel(MTLSTR("nxemu null 2d msaa"));
            runtime->device.AddResidentAllocation(ms_backing.get());
            if (Owned<MTL::Texture> v2d = Adopt(ms_backing->newTextureView(
                    MTL::PixelFormatRGBA8Unorm, MTL::TextureType2DMultisample,
                    NS::Range::Make(0, 1), NS::Range::Make(0, 1)))) {
                runtime->device.AddResidentAllocation(v2d.get());
                msaa_views[0] = std::move(v2d);
            }
            if (ms_backing->textureType() == MTL::TextureType2DMultisampleArray) {
                if (Owned<MTL::Texture> v2da = Adopt(ms_backing->newTextureView(
                        MTL::PixelFormatRGBA8Unorm, MTL::TextureType2DMultisampleArray,
                        NS::Range::Make(0, 1), NS::Range::Make(0, 1)))) {
                    runtime->device.AddResidentAllocation(v2da.get());
                    msaa_views[1] = std::move(v2da);
                }
            }
            null_backing_textures[2] = std::move(ms_backing);
        }
    }

    pixel_format = MTL::PixelFormatRGBA8Unorm;
    sampled_format = MTL::PixelFormatRGBA8Unorm;
    view_attempted = ~0u;
    msaa_view_attempted = ~0u;
    runtime->device.CommitResidency();
}

ImageView::~ImageView() {
    // Every view MakeView/MakeArrayFromVolume built went through
    // AddResidentAllocation. A view is its own MTL::Allocation with its own
    // gpuResourceID, the parent being resident doesn't cover it. Releasing it
    // here without telling the device leaks a residency entry per view, and the
    // set is rebuilt from those entries, so wired memory climbs for as long as
    // the game runs. On an 8 GiB M1 that ends as a GPU-side stall with every
    // in-flight buffer discarded along with it.
    //
    // Deferred like Image::~Image and for the same reason, a Metal 4 command
    // buffer holds no strong reference to what it touches, so a view dropped
    // while a recorded draw still samples it is a use-after-free. Passing the
    // texture as both object and allocation makes the sweep drop the residency
    // entry when the tick retires.
    if (runtime == nullptr) {
        return;
    }
    runtime->BumpViewGeneration();
    const auto retire = [this](const Owned<MTL::Texture>& view) {
        if (view) {
            runtime->scheduler.KeepAlive(view.get(), view.get());
        }
    };
    retire(render_target);
    retire(volume_copy);
    retire(msaa_copy);
    retire(depth_view);
    retire(stencil_view);
    for (const Owned<MTL::Texture>& view : views) {
        retire(view);
    }
    for (const Owned<MTL::Texture>& view : msaa_views) {
        retire(view);
    }
    for (const Owned<MTL::Texture>& backing : null_backing_textures) {
        retire(backing);
    }
    for (const auto& [format, view] : storage_views) {
        retire(view);
    }
}

MTL::Texture* ImageView::MakeView(MTL::PixelFormat format, MTL::TextureType texture_type,
                                  u32 first_level, u32 num_levels, u32 first_layer,
                                  u32 num_layers,
                                  const MTL::TextureSwizzleChannels* swizzle) {
    if (image_handle == nullptr) {
        return nullptr;
    }

    // Metal ABORTS the process inside newTextureView if the level or slice range
    // steps outside the base texture. No nil return, no validation message. The
    // guest can legitimately describe a wider view, so we clamp every range
    // below instead of trusting it.
    const u32 total_levels = static_cast<u32>(image_handle->mipmapLevelCount());
    const MTL::TextureType base_type = image_handle->textureType();
    const bool base_is_cube = base_type == MTL::TextureTypeCube ||
                              base_type == MTL::TextureTypeCubeArray;
    // Has to be decided BEFORE the slice range check below. A volume keeps its
    // slices in depth and reports arrayLength 1, so the guest's slice index
    // always looks out of range and we'd drop the view. Widening the check isn't
    // an option, a 3D image viewed as 3D still needs a slice range of {0, 1} or
    // Metal aborts.
    if (base_type == MTL::TextureType3D &&
        (texture_type == MTL::TextureType2DArray || texture_type == MTL::TextureType2D)) {
        if (first_level >= total_levels) {
            return nullptr;
        }
        return MakeArrayFromVolume(format, texture_type, first_level, num_levels, first_layer);
    }

    const u32 total_slices =
        static_cast<u32>(image_handle->arrayLength()) * (base_is_cube ? 6u : 1u);

    if (first_level >= total_levels || first_layer >= total_slices) {
        LOG_WARNING(Render_Metal,
                    "Texture view starts outside its image (level {}/{}, slice {}/{}) - base "
                    "{}x{}x{} type={} fmt={} gpu_addr=0x{:x}",
                    first_level, total_levels, first_layer, total_slices,
                    static_cast<u32>(image_handle->width()),
                    static_cast<u32>(image_handle->height()),
                    static_cast<u32>(image_handle->depth()), static_cast<u32>(base_type),
                    static_cast<u32>(image_handle->pixelFormat()), gpu_addr);
        return nullptr;
    }
    num_levels = (std::min)(num_levels, total_levels - first_level);
    num_layers = (std::min)(num_layers, total_slices - first_layer);

    // Non-array Metal view types abort inside newTextureView if given >1 slice (or !=6 for Cube).
    switch (texture_type) {
    case MTL::TextureType1D:
    case MTL::TextureType2D:
    case MTL::TextureType2DMultisample:
    case MTL::TextureType3D:
        num_layers = (std::min)(num_layers, 1u);
        break;
    case MTL::TextureTypeCube:
        num_layers = (std::min)(num_layers, 6u);
        break;
    default:
        break;
    }

    // A cube view needs six faces per cube and square dimensions; CubeArray
    // requires a multiple of six slices, or Metal aborts inside newTextureView.
    const bool view_is_cube = texture_type == MTL::TextureTypeCube ||
                              texture_type == MTL::TextureTypeCubeArray;
    if (view_is_cube) {
        if (num_layers < 6 || image_handle->width() != image_handle->height()) {
            static std::atomic<u32> reported{0};
            if (reported++ < 8) {
                LOG_WARNING(Render_Metal,
                            "Cube view invalid (slices={}, base={}x{}x{} type={} levels={} fmt={} "
                            "gpu_addr=0x{:x})",
                            num_layers, static_cast<u32>(image_handle->width()),
                            static_cast<u32>(image_handle->height()),
                            static_cast<u32>(image_handle->depth()), static_cast<u32>(base_type),
                            static_cast<u32>(image_handle->mipmapLevelCount()),
                            static_cast<u32>(image_handle->pixelFormat()), gpu_addr);
            }
            return nullptr;
        }
        if (texture_type == MTL::TextureTypeCubeArray) {
            num_layers = (num_layers / 6u) * 6u;
        }
    }

    // A view's type has to be in the same view class as the base texture, and an
    // incompatible pair aborts instead of returning nil, so check it first.
    if (ViewClass(base_type) != ViewClass(texture_type)) {
        // A 3D image sampled as a 2D array is common enough that we serve it by
        // copying the depth slices instead of dropping the view. The copy is a
        // snapshot, fine for the LUTs this hits but not for a volume the guest
        // keeps writing.
        if (base_type == MTL::TextureType3D && (texture_type == MTL::TextureType2DArray ||
                                                texture_type == MTL::TextureType2D)) {
            return MakeArrayFromVolume(format, texture_type, first_level, num_levels, first_layer);
        }
        if (base_type == MTL::TextureType2DMultisample && texture_type == MTL::TextureType2D) {
            return Make2DFromMultisample(format, swizzle);
        }
        static std::atomic<u32> reported{0};
        if (reported++ < 8) {
            LOG_WARNING(Render_Metal,
                        "Incompatible texture view type {} over base {} - {}x{}x{} levels={} "
                        "layers={} fmt={} gpu_addr=0x{:x}",
                        static_cast<u32>(texture_type), static_cast<u32>(base_type),
                        static_cast<u32>(image_handle->width()),
                        static_cast<u32>(image_handle->height()),
                        static_cast<u32>(image_handle->depth()),
                        static_cast<u32>(image_handle->mipmapLevelCount()),
                        static_cast<u32>(image_handle->arrayLength()),
                        static_cast<u32>(image_handle->pixelFormat()), gpu_addr);
        }
        return nullptr;
    }
    if (num_levels == 0 || num_layers == 0) {
        return nullptr;
    }

    if (swizzle != nullptr) {
        MTL::Texture* const view = image_handle->newTextureView(
            format, texture_type, NS::Range::Make(first_level, num_levels),
            NS::Range::Make(first_layer, num_layers), *swizzle);
        if (view != nullptr) {
            // A view is its own MTL::Allocation with its own gpuResourceID and
            // that ID is what the shader addresses, the parent being resident
            // doesn't cover it. Metal 4 makes nothing implicitly resident, so an
            // unregistered view is a GPU page fault that discards the whole
            // submission and everything drawn in it.
            if (runtime != nullptr) {
                runtime->device.AddResidentAllocation(view);
            }
            return view;
        }
        // Not every format takes a component mapping, depth especially. Falling
        // back to identity samples the wrong channels, but returning null would
        // make Handle() hand the shader a different texture entirely.
        static std::atomic<u32> reported{0};
        if (reported++ < 4) {
            LOG_WARNING(Render_Metal, "Swizzled view rejected for format {}; using identity",
                        static_cast<u32>(format));
        }
    }
    MTL::Texture* const view =
        image_handle->newTextureView(format, texture_type, NS::Range::Make(first_level, num_levels),
                                     NS::Range::Make(first_layer, num_layers));
    if (view != nullptr && runtime != nullptr) {
        // See the swizzled path above, the view has to be in the residency set
        // and not just its parent.
        runtime->device.AddResidentAllocation(view);
    }
    return view;
}

MTL::Texture* ImageView::MakeArrayFromVolume(MTL::PixelFormat format,
                                             MTL::TextureType target_type, u32 first_level,
                                             u32 num_levels, u32 first_layer) {
    ScopedAutoreleasePool pool;

    const u32 width = static_cast<u32>(image_handle->width());
    const u32 height = static_cast<u32>(image_handle->height());
    const u32 depth = static_cast<u32>(image_handle->depth());
    if (width == 0 || height == 0 || depth == 0) {
        return nullptr;
    }
    const u32 max_levels = static_cast<u32>(image_handle->mipmapLevelCount());
    if (first_level >= max_levels) {
        return nullptr;
    }
    num_levels = (std::min)(num_levels, max_levels - first_level);

    const u32 base_width = (std::max)(width >> first_level, 1u);
    const u32 base_height = (std::max)(height >> first_level, 1u);
    const u32 base_depth = (std::max)(depth >> first_level, 1u);
    const u32 clamped_first_layer = (std::min)(first_layer, base_depth - 1);
    const u32 array_length =
        (std::max)(1u, base_depth - clamped_first_layer);

    const bool is_single_2d = target_type == MTL::TextureType2D;
    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setTextureType(is_single_2d ? MTL::TextureType2D : MTL::TextureType2DArray);
    descriptor->setPixelFormat(format);
    descriptor->setWidth(base_width);
    descriptor->setHeight(base_height);
    descriptor->setMipmapLevelCount(num_levels);
    descriptor->setArrayLength(is_single_2d ? 1 : array_length);
    descriptor->setUsage(MTL::TextureUsageShaderRead);
    // Made here instead of through MemoryAllocator::CreateTexture because
    // MakeView has to return a +1 object and Owned is a shared pointer with no
    // release(). The storage mode is what the allocator would have picked. The
    // hazard mode does nothing, Metal 4 treats every resource as untracked and
    // the allocator sets none.
    descriptor->setStorageMode(runtime->memory_allocator.StorageModeFor(MemoryUsage::DeviceLocal));
    descriptor->setHazardTrackingMode(MTL::HazardTrackingModeTracked);

    MTL::Texture* const copy = runtime->device.GetDevice()->newTexture(descriptor);
    if (copy != nullptr) {
        runtime->device.AddResidentAllocation(copy);
        runtime->device.CommitResidency();
    }
    descriptor->release();
    if (copy == nullptr) {
        LOG_WARNING(Render_Metal, "Could not allocate a {}x{}x{} array copy of a 3D image",
                    base_width, base_height, array_length);
        return nullptr;
    }
    copy->setLabel(MTLSTR("eden 3D-as-2D-array"));

    // Kept so we can refresh the copy. A volume the guest re-renders every frame
    // would otherwise be sampled as a frame-0 snapshot forever.
    volume_copy_format = format;
    volume_copy_first_level = first_level;
    volume_copy_num_levels = num_levels;
    volume_copy_first_layer = clamped_first_layer;
    volume_copy_mod_tick = (slot_images != nullptr && image_id)
                               ? (*slot_images)[image_id].modification_tick
                               : runtime->FrameIndex();

    MTL::Texture* const source = image_handle;
    // Ends the pass on the recording side too. RequestBlit closes the render
    // encoder on the worker, but without this the recording thread's mirror
    // still thinks a pass is open and every later check of that is wrong.
    runtime->scheduler.RequestOutsideRenderPassOperationContext();
    runtime->scheduler.Record([source, copy, width, height, depth, first_level, num_levels,
                               clamped_first_layer, array_length,
                               is_single_2d](CommandContext& context) {
        BlitEncoder encoder = context.RequestBlit();
        if (encoder == nullptr) {
            return;
        }
        for (u32 level = 0; level < num_levels; ++level) {
            const u32 shift = first_level + level;
            const u32 level_width = (std::max)(width >> shift, 1u);
            const u32 level_height = (std::max)(height >> shift, 1u);
            const u32 level_depth = (std::max)(depth >> shift, 1u);
            if (is_single_2d) {
                const u32 slice = (std::min)(clamped_first_layer, level_depth - 1);
                encoder->copyFromTexture(
                    source, 0, shift, MTL::Origin::Make(0, 0, slice),
                    AlignCopySizeToBlock(MTL::Size::Make(level_width, level_height, 1),
                                         source->pixelFormat()),
                    copy, 0, level, MTL::Origin::Make(0, 0, 0));
            } else {
                const u32 start_slice = (std::min)(clamped_first_layer, level_depth - 1);
                const u32 slices = (std::min)(array_length, level_depth - start_slice);
                for (u32 slice = 0; slice < slices; ++slice) {
                    // A 3D texture has one slice and addresses depth through the
                    // origin, the destination does it through the slice index.
                    encoder->copyFromTexture(
                        source, 0, shift, MTL::Origin::Make(0, 0, start_slice + slice),
                        AlignCopySizeToBlock(MTL::Size::Make(level_width, level_height, 1),
                                             source->pixelFormat()),
                        copy, slice, level, MTL::Origin::Make(0, 0, 0));
                }
            }
        }
    });

    static std::atomic<u32> made{0};
    if (made++ == 0) {
        LOG_INFO(Render_Metal, "Copying 3D images into 2D/2D-array textures for sampling");
    }
    return copy;
}

void ImageView::RefreshVolumeCopy(MTL::Texture* destination) const {
    if (destination == nullptr || image_handle == nullptr ||
        volume_copy_num_levels == 0) {
        return;
    }
    const u32 width = static_cast<u32>(image_handle->width());
    const u32 height = static_cast<u32>(image_handle->height());
    const u32 depth = static_cast<u32>(image_handle->depth());
    MTL::Texture* const source = image_handle;
    const u32 first_level = volume_copy_first_level;
    const u32 num_levels = volume_copy_num_levels;
    const u32 first_layer = volume_copy_first_layer;
    const u32 dst_layers = static_cast<u32>(destination->arrayLength());
    const bool is_single_2d = destination->textureType() == MTL::TextureType2D;
    // Ends the pass on the recording side too. RequestBlit closes the render
    // encoder on the worker, but without this the recording thread's mirror
    // still thinks a pass is open and every later check of that is wrong.
    runtime->scheduler.RequestOutsideRenderPassOperationContext();
    runtime->scheduler.Record([source, destination, width, height, depth, first_level, num_levels,
                               first_layer, dst_layers, is_single_2d](CommandContext& context) {
        BlitEncoder encoder = context.RequestBlit();
        if (encoder == nullptr) {
            return;
        }
        for (u32 level = 0; level < num_levels; ++level) {
            const u32 shift = first_level + level;
            const u32 level_width = (std::max)(width >> shift, 1u);
            const u32 level_height = (std::max)(height >> shift, 1u);
            const u32 level_depth = (std::max)(depth >> shift, 1u);
            if (is_single_2d) {
                const u32 slice = (std::min)(first_layer, level_depth - 1);
                encoder->copyFromTexture(
                    source, 0, shift, MTL::Origin::Make(0, 0, slice),
                    AlignCopySizeToBlock(MTL::Size::Make(level_width, level_height, 1),
                                         source->pixelFormat()),
                    destination, 0, level, MTL::Origin::Make(0, 0, 0));
            } else {
                const u32 start_slice = (std::min)(first_layer, level_depth - 1);
                const u32 slices = (std::min)(dst_layers, level_depth - start_slice);
                for (u32 slice = 0; slice < slices; ++slice) {
                    encoder->copyFromTexture(
                        source, 0, shift, MTL::Origin::Make(0, 0, start_slice + slice),
                        AlignCopySizeToBlock(MTL::Size::Make(level_width, level_height, 1),
                                             source->pixelFormat()),
                        destination, slice, level, MTL::Origin::Make(0, 0, 0));
                }
            }
        }
    });
}

MTL::Texture* ImageView::Make2DFromMultisample(MTL::PixelFormat format,
                                               const MTL::TextureSwizzleChannels* swizzle) {
    if (runtime == nullptr || image_handle == nullptr) {
        return nullptr;
    }
    ScopedAutoreleasePool pool;

    const u32 width = static_cast<u32>(image_handle->width());
    const u32 height = static_cast<u32>(image_handle->height());
    if (width == 0 || height == 0) {
        return nullptr;
    }
    const MTL::PixelFormat base_format = image_handle->pixelFormat();
    const bool needs_view = format != base_format || swizzle != nullptr;

    MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
    descriptor->setTextureType(MTL::TextureType2D);
    descriptor->setPixelFormat(base_format);
    descriptor->setWidth(width);
    descriptor->setHeight(height);
    descriptor->setMipmapLevelCount(1);
    descriptor->setArrayLength(1);
    descriptor->setSampleCount(1);
    MTL::TextureUsage usage = MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead;
    if (needs_view) {
        usage |= MTL::TextureUsagePixelFormatView;
    }
    descriptor->setUsage(usage);
    Owned<MTL::Texture> resolved =
        runtime->memory_allocator.CreateTexture(descriptor, MemoryUsage::DeviceLocal);
    descriptor->release();
    if (!resolved) {
        LOG_WARNING(Render_Metal, "Could not allocate a {}x{} 2D resolve copy of an MSAA image",
                    width, height);
        return nullptr;
    }
    resolved->setLabel(MTLSTR("nxemu MSAA-as-2D"));

    msaa_copy = std::move(resolved);
    msaa_copy_mod_tick = (slot_images != nullptr && image_id)
                             ? (*slot_images)[image_id].modification_tick
                             : runtime->FrameIndex();
    msaa_copy_write_tick = (slot_images != nullptr && image_id)
                               ? (*slot_images)[image_id].attachment_write_tick
                               : runtime->scheduler.RenderPassEndTick();
    RefreshMsaaCopy();

    if (needs_view) {
        MTL::Texture* view = nullptr;
        if (swizzle != nullptr) {
            view = msaa_copy->newTextureView(format, MTL::TextureType2D, NS::Range::Make(0, 1),
                                             NS::Range::Make(0, 1), *swizzle);
        }
        if (view == nullptr) {
            view = msaa_copy->newTextureView(format, MTL::TextureType2D, NS::Range::Make(0, 1),
                                             NS::Range::Make(0, 1));
        }
        if (view != nullptr) {
            runtime->device.AddResidentAllocation(view);
            return view;
        }
    }
    return msaa_copy->retain();
}

void ImageView::RefreshMsaaCopy() const {
    if (runtime == nullptr || image_handle == nullptr || !msaa_copy) {
        return;
    }
    const RenderPassState resolve_pass = BuildMsaaResolvePass(image_handle, msaa_copy.get());
    runtime->scheduler.RequestRenderPass(resolve_pass);
    runtime->scheduler.RequestOutsideRenderPassOperationContext();
}

MTL::Texture* ImageView::MakeViewForType(Shader::TextureType texture_type) const {
    // Layer count follows the requested type, same as the constructor's switch.
    // A single-layer type takes one, an array type takes the view's whole range,
    // and a cube needs its six faces.
    u32 num_layers = 1;
    switch (texture_type) {
    case Shader::TextureType::ColorArray1D:
    case Shader::TextureType::ColorArray2D:
    case Shader::TextureType::Color3D:
    case Shader::TextureType::ColorArrayCube:
    case Shader::TextureType::Buffer:
        num_layers = range.extent.layers;
        break;
    case Shader::TextureType::ColorCube:
        num_layers = 6;
        break;
    default:
        break;
    }
    return const_cast<ImageView*>(this)->MakeView(
        sampled_format, ToTextureType(texture_type), range.base.level, range.extent.levels,
        range.base.layer, num_layers, view_swizzle_identity ? nullptr : &view_swizzle);
}

MTL::Texture* ImageView::Handle(Shader::TextureType texture_type,
                                bool is_multisample) const noexcept {
    if (is_multisample) {
        const size_t ms_idx = texture_type == Shader::TextureType::ColorArray2D ? 1u : 0u;
        const u32 attempt_bit = 1u << ms_idx;
        if (!msaa_views[ms_idx] && image_handle != nullptr &&
            (msaa_view_attempted & attempt_bit) == 0) {
            msaa_view_attempted |= attempt_bit;
            const MTL::TextureType ms_type =
                ms_idx == 1u ? MTL::TextureType2DMultisampleArray : MTL::TextureType2DMultisample;
            const u32 num_layers = ms_idx == 1u ? range.extent.layers : 1u;
            const MTL::TextureSwizzleChannels* const swizzle =
                view_swizzle_identity ? nullptr : &view_swizzle;
            msaa_views[ms_idx] = Adopt(const_cast<ImageView*>(this)->MakeView(
                sampled_format, ms_type, 0, 1, range.base.layer, num_layers, swizzle));
        }
        if (msaa_views[ms_idx]) {
            return msaa_views[ms_idx].get();
        }
        return nullptr;
    }
    const size_t index = static_cast<size_t>(texture_type);
    if (index >= views.size()) {
        return nullptr;
    }
    const u32 attempt_bit = 1u << index;
    if (!views[index] && image_handle != nullptr && (view_attempted & attempt_bit) == 0) {
        // Build the type the shader wants, but only once. The image can't change
        // shape, so a type that fails to view now always will.
        view_attempted |= attempt_bit;
        views[index] = Adopt(MakeViewForType(texture_type));
    }
    if (views[index]) {
        // The 2D-array stand-in is a copy and goes stale whenever the guest modifies
        // the underlying 3D volume (including mid-frame updates). Track the parent
        // image's modification_tick so dynamic volumes always refresh promptly and
        // static 3D LUTs are never copied redundantly.
        if (volume_copy_num_levels != 0 &&
            (texture_type == Shader::TextureType::ColorArray2D ||
             texture_type == Shader::TextureType::Color2D)) {
            const u64 mod_tick = (slot_images != nullptr && image_id)
                                     ? (*slot_images)[image_id].modification_tick
                                     : runtime->FrameIndex();
            const u64 write_tick = (slot_images != nullptr && image_id)
                                       ? (*slot_images)[image_id].attachment_write_tick
                                       : 0;
            if (volume_copy_mod_tick != mod_tick || volume_copy_write_tick != write_tick) {
                volume_copy_mod_tick = mod_tick;
                volume_copy_write_tick = write_tick;
                RefreshVolumeCopy(views[index].get());
            }
        }
        if (msaa_copy && texture_type == Shader::TextureType::Color2D) {
            const u64 mod_tick = (slot_images != nullptr && image_id)
                                     ? (*slot_images)[image_id].modification_tick
                                     : runtime->FrameIndex();
            const u64 write_tick = (slot_images != nullptr && image_id)
                                       ? (*slot_images)[image_id].attachment_write_tick
                                       : runtime->scheduler.RenderPassEndTick();
            if (msaa_copy_mod_tick != mod_tick || msaa_copy_write_tick != write_tick) {
                msaa_copy_mod_tick = mod_tick;
                msaa_copy_write_tick = write_tick;
                RefreshMsaaCopy();
            }
        }
        return views[index].get();
    }
    // Nothing of the requested type could be built, so the shader gets NOTHING.
    // Do NOT substitute another view of the image, Metal won't reject a 2D
    // handle where the shader declared a cube, it just samples garbage. Null
    // reads as zero, same as what Vulkan gives here.
    static std::atomic<u32> reported{0};
    if (reported++ < 12) {
        LOG_WARNING(Render_Metal,
                    "No view for shader texture type {}; binding nothing, so it reads zero "
                    "- base {}x{}x{} type={} levels={} layers={} "
                    "fmt={} samples={} view_type={} range(level {}+{}, layer {}+{}) gpu_addr=0x{:x}",
                    index, image_handle != nullptr ? image_handle->width() : 0,
                    image_handle != nullptr ? image_handle->height() : 0,
                    image_handle != nullptr ? image_handle->depth() : 0,
                    image_handle != nullptr ? static_cast<u32>(image_handle->textureType()) : 0u,
                    image_handle != nullptr ? image_handle->mipmapLevelCount() : 0,
                    image_handle != nullptr ? image_handle->arrayLength() : 0,
                    image_handle != nullptr ? static_cast<u32>(image_handle->pixelFormat()) : 0u,
                    image_handle != nullptr ? image_handle->sampleCount() : 0,
                    static_cast<u32>(type), range.base.level, range.extent.levels,
                    range.base.layer, range.extent.layers, gpu_addr);
    }
    return nullptr;
}

MTL::Texture* ImageView::StorageView(Shader::TextureType texture_type,
                                     Shader::ImageFormat image_format) {
    if (image_handle == nullptr) {
        return nullptr;
    }
    const MTL::PixelFormat base_format = image_handle->pixelFormat();
    MTL::PixelFormat target_format = StripSrgbFormat(base_format);
    const PixelFormat desired_format =
        image_format == Shader::ImageFormat::Typeless ? format : StorageFormatOf(image_format);
    if (desired_format != PixelFormat::Invalid) {
        const FormatInfo info = SurfaceFormat(runtime->device, desired_format);
        if (info.support != FormatSupport::Unsupported &&
            AreMetalPixelFormatsViewCompatible(base_format, info.format)) {
            target_format = StripSrgbFormat(info.format);
        }
    }
    if (!AreMetalPixelFormatsViewCompatible(base_format, target_format)) {
        return nullptr;
    }
    // Always cache a non-swizzled, non-sRGB view for storage writes: Metal rejects
    // ShaderWrite on swizzled or sRGB texture views.
    const u32 key = (static_cast<u32>(image_format) << 8) | static_cast<u32>(texture_type);
    if (const auto it = storage_views.find(key); it != storage_views.end()) {
        return it->second.get();
    }
    Owned<MTL::Texture> view =
        Adopt(MakeView(target_format, ToTextureType(texture_type), range.base.level,
                       range.extent.levels, range.base.layer, range.extent.layers, nullptr));
    if (!view) {
        return nullptr;
    }
    MTL::Texture* const raw = view.get();
    storage_views.emplace(key, std::move(view));
    return raw;
}

MTL::Texture* ImageView::ColorView() {
    return render_target.get();
}

MTL::Texture* ImageView::DepthView() {
    if (depth_view) {
        return depth_view.get();
    }
    if (image_handle == nullptr) {
        return nullptr;
    }
    depth_view = Adopt(MakeView(pixel_format, image_handle->textureType(), range.base.level,
                                range.extent.levels, range.base.layer, range.extent.layers,
                                nullptr));
    return depth_view.get();
}

MTL::Texture* ImageView::StencilView() {
    if (stencil_view) {
        return stencil_view.get();
    }
    if (image_handle == nullptr) {
        return nullptr;
    }
    // A packed depth/stencil texture is viewed as stencil-only by asking for
    // the X<n>_Stencil8 companion format; Stencil8 uses itself directly.
    MTL::PixelFormat stencil_format = MTL::PixelFormatInvalid;
    switch (pixel_format) {
    case MTL::PixelFormatStencil8:
        stencil_format = MTL::PixelFormatStencil8;
        break;
    case MTL::PixelFormatDepth32Float_Stencil8:
        stencil_format = MTL::PixelFormatX32_Stencil8;
        break;
    case MTL::PixelFormatDepth24Unorm_Stencil8:
        stencil_format = MTL::PixelFormatX24_Stencil8;
        break;
    default:
        return nullptr;
    }
    stencil_view = Adopt(MakeView(stencil_format, image_handle->textureType(), range.base.level,
                                  range.extent.levels, range.base.layer, range.extent.layers,
                                  nullptr));
    return stencil_view.get();
}

bool ImageView::IsRescaled() const noexcept {
    // Ask the image instead of caching it. A view outlives the decision, the
    // cache toggles resolution before invalidating what pointed at it.
    if (slot_images == nullptr) {
        return false;
    }
    return (*slot_images)[image_id].IsRescaled();
}

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

Sampler::Sampler(TextureCacheRuntime& runtime, const Tegra::Texture::TSCEntry& tsc) {
    runtime.BumpViewGeneration();
    ScopedAutoreleasePool pool;

    MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();
    // Needed before a sampler can be referenced from an argument buffer, only a
    // sampler made with this flag has a usable gpuResourceID. Without it the
    // shader samples through an invalid handle and reads black instead of
    // failing.
    descriptor->setSupportArgumentBuffers(true);
    descriptor->setMinFilter(SamplerFilter(tsc.min_filter));
    descriptor->setMagFilter(SamplerFilter(tsc.mag_filter));
    descriptor->setMipFilter(SamplerMipFilter(tsc.mipmap_filter));
    // The MAGNIFICATION filter decides how WrapMode::Clamp resolves, matching
    // Vulkan. Passing the minification filter picks the wrong address mode for
    // any sampler whose two filters differ.
    descriptor->setSAddressMode(WrapMode(runtime.device, tsc.wrap_u, tsc.mag_filter));
    descriptor->setTAddressMode(WrapMode(runtime.device, tsc.wrap_v, tsc.mag_filter));
    descriptor->setRAddressMode(WrapMode(runtime.device, tsc.wrap_p, tsc.mag_filter));
    // Only meaningful with ClampToBorderColor. Metal's default is transparent
    // black, which makes every out-of-bounds shadow lookup fully shadowed.
    descriptor->setBorderColor(BorderColorFor(tsc.BorderColor()));
    // With mipmapping off the guest still leaves its LOD clamps filled in, and
    // honouring them can pick a mip it never meant to sample. Pinned to
    // [0, 0.25] to match Vulkan.
    const bool mipmapped =
        tsc.mipmap_filter != Tegra::Texture::TextureMipmapFilter::None;
    descriptor->setLodMinClamp(mipmapped ? tsc.MinLod() : 0.0f);
    descriptor->setLodMaxClamp(mipmapped ? tsc.MaxLod() : 0.25f);
    // Metal has no sampler LOD bias, MSL takes it as an argument to the sample
    // call instead, so a guest that biases mip selection doesn't get it.
    if (tsc.LodBias() != 0.0f) {
        static std::atomic<u32> reported{0};
        if (reported++ == 0) {
            LOG_DEBUG(Render_Metal,
                      "Sampler LOD bias {} ignored; Metal has no sampler-level bias",
                      tsc.LodBias());
        }
    }
    // Redundant, already set above.
    descriptor->setSupportArgumentBuffers(true);

    if (tsc.depth_compare_enabled != 0) {
        descriptor->setCompareFunction(DepthCompareFunction(tsc.depth_compare_func));
    }

    // Maxwell can ask for the min or max of the footprint instead of a weighted
    // average, which is how min/max depth pyramids get built. Averaging those
    // gives back a depth no surface is at. Mutually exclusive with depth
    // comparison like in Vulkan, comparison wins.
    //
    // Only ever requested, never assumed. Min/max reduction needs device support
    // this hardware doesn't have, and asking anyway doesn't degrade, it makes
    // newSamplerState return NOTHING. The validation layer says so outright
    // ("Use of minimum or maximum reductionMode requires device support"),
    // without it the failure is silent and the caller gets a null sampler. That
    // null skips the argument table bind, so the slot keeps pointing at whatever
    // sampler was there before, and a shader sampling through a stale id page
    // faults, which discards the submission and its neighbours. Weighted average
    // is a small visual compromise, no sampler at all is not.
    const auto reduction = tsc.reduction_filter.Value();
    if (reduction != Tegra::Texture::SamplerReduction::WeightedAverage &&
        !runtime.device.SupportsSamplerReduction()) {
        static std::atomic<u32> unsupported{0};
        if (unsupported++ == 0) {
            LOG_WARNING(Render_Metal,
                        "Sampler reduction mode {} is unsupported on this device; using a "
                        "weighted average",
                        static_cast<u32>(reduction));
        }
    } else if (reduction != Tegra::Texture::SamplerReduction::WeightedAverage) {
        if (tsc.depth_compare_enabled == 0) {
            descriptor->setReductionMode(SamplerReductionMode(reduction));
        } else {
            static std::atomic<u32> reported{0};
            if (reported++ == 0) {
                LOG_DEBUG(Render_Metal,
                          "Sampler asks for reduction mode {} together with depth comparison; "
                          "keeping the comparison",
                          static_cast<u32>(reduction));
            }
        }
    }

    const u32 max_anisotropy =
        std::clamp(static_cast<u32>(tsc.MaxAnisotropy()), 1U, 16U);
    descriptor->setMaxAnisotropy(max_anisotropy);

    has_linear_filtering = tsc.min_filter == Tegra::Texture::TextureFilter::Linear ||
                           tsc.mag_filter == Tegra::Texture::TextureFilter::Linear;
    has_depth_comparison = tsc.depth_compare_enabled != 0;

    sampler = Adopt(runtime.device.GetDevice()->newSamplerState(descriptor));
    if (!sampler) {
        // Never silent. A null sampler isn't bound at all, so the shader keeps
        // sampling through whichever sampler last held that argument table slot.
        // See the reduction-mode note above for how that ends.
        static std::atomic<u32> failed{0};
        const u32 n = failed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4 || n % 500 == 0) {
            LOG_ERROR(Render_Metal,
                      "newSamplerState returned nothing ({} so far); the shader will sample "
                      "through a stale argument table slot",
                      n);
        }
    }

    // Fallback carrying the GUEST's own anisotropy. TSCEntry::MaxAnisotropy()
    // already folds the user's forced setting in, but on shadow maps and
    // single/dual-channel data textures that's a wrong result and not extra
    // quality. SamplerFor picks between the two per draw.
    const u32 guest_anisotropy = std::clamp(1U << tsc.max_anisotropy, 1U, 16U);
    if (max_anisotropy > guest_anisotropy) {
        descriptor->setMaxAnisotropy(guest_anisotropy);
        sampler_default_anisotropy =
            Adopt(runtime.device.GetDevice()->newSamplerState(descriptor));
        descriptor->setMaxAnisotropy(max_anisotropy);
    }

    // Variants for pairings the guest can ask for and Metal leaves undefined.
    // Built here because a sampler state is immutable, so picking one at bind
    // time means we had to make it up front.
    if (has_linear_filtering) {
        // Used exclusively when sampling integer textures, where linear filtering,
        // depth comparison, and anisotropy are all invalid in Metal.
        descriptor->setMinFilter(MTL::SamplerMinMagFilterNearest);
        descriptor->setMagFilter(MTL::SamplerMinMagFilterNearest);
        descriptor->setMipFilter(MTL::SamplerMipFilterNearest);
        descriptor->setCompareFunction(MTL::CompareFunctionNever);
        descriptor->setMaxAnisotropy(1);
        sampler_nearest = Adopt(runtime.device.GetDevice()->newSamplerState(descriptor));
        descriptor->setMinFilter(SamplerFilter(tsc.min_filter));
        descriptor->setMagFilter(SamplerFilter(tsc.mag_filter));
        descriptor->setMipFilter(SamplerMipFilter(tsc.mipmap_filter));
        descriptor->setMaxAnisotropy(max_anisotropy);
        if (has_depth_comparison) {
            descriptor->setCompareFunction(DepthCompareFunction(tsc.depth_compare_func));
        }
    }
    if (has_depth_comparison) {
        descriptor->setCompareFunction(MTL::CompareFunctionNever);
        descriptor->setMaxAnisotropy(1);
        sampler_no_compare = Adopt(runtime.device.GetDevice()->newSamplerState(descriptor));
    }

    descriptor->release();
}

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------

Framebuffer::Framebuffer(TextureCacheRuntime& runtime,
                         std::span<ImageView*, NUM_RT> color_buffers, ImageView* depth_buffer,
                         const RenderTargets& key) {
    CreateFramebuffer(runtime, color_buffers, depth_buffer, key.is_rescaled);
    ClampRenderArea(key.size.width, key.size.height);
}

Framebuffer::Framebuffer(TextureCacheRuntime& runtime, ImageView* color_buffer,
                         ImageView* depth_buffer, u32 width, u32 height, bool is_rescaled_)
    : is_rescaled{is_rescaled_} {
    std::array<ImageView*, NUM_RT> color_buffers{};
    color_buffers[0] = color_buffer;
    CreateFramebuffer(runtime, color_buffers, depth_buffer, is_rescaled_);
    ClampRenderArea(width, height);
}

Framebuffer::~Framebuffer() = default;

void Framebuffer::ClampRenderArea(u32 width, u32 height) {
    // Never larger than the smallest attachment. This becomes the pass
    // descriptor's renderTargetWidth/Height, and declaring more than a texture
    // has is out of bounds, Metal won't trim it. It also bounds every draw's
    // scissor, so getting it wrong clips geometry away.
    //
    // Measured from the image VIEWS and not the Metal textures, a view over a
    // mip range doesn't necessarily report that level's dimensions.
    pass_state.render_area_width = (std::min)(width, attachment_width);
    pass_state.render_area_height = (std::min)(height, attachment_height);

    // Deduplicated by the sizes involved, so a mismatch reports once per shape
    // instead of once per pass.
    if (attachment_width < width || attachment_height < height) {
        static std::mutex seen_mutex;
        static std::set<u64> seen;
        const u64 key = (static_cast<u64>(width) << 48) | (static_cast<u64>(height) << 32) |
                        (static_cast<u64>(attachment_width) << 16) |
                        static_cast<u64>(attachment_height);
        bool first = false;
        {
            std::scoped_lock lock{seen_mutex};
            first = seen.insert(key).second;
        }
        if (first) {
            LOG_WARNING(Render_Metal,
                        "Render area clamped: guest asked for {}x{}, attachments resolved "
                        "{}x{} - the difference is clipped, not scaled",
                        width, height, attachment_width, attachment_height);
        }
    }
}

void Framebuffer::CreateFramebuffer(TextureCacheRuntime& runtime,
                                    std::span<ImageView*, NUM_RT> color_buffers,
                                    ImageView* depth_buffer, bool is_rescaled_) {
    is_rescaled = is_rescaled_;
    pass_state = RenderPassState{};
    color_formats.fill(MTL::PixelFormatInvalid);
    attachment_images = {};
    num_attachment_images = 0;
    attachment_width = std::numeric_limits<u32>::max();
    attachment_height = std::numeric_limits<u32>::max();
    // Attachment extents, and how many array layers the pass renders into. The
    // layer count has to be the max over the attachments' view ranges. Left at
    // one, Metal renders every draw into slice zero no matter what the vertex
    // shader writes to [[render_target_array_index]].
    u32 layers = 1;
    const auto fit = [&, this](const ImageView* view) {
        // ImageViewBase::size is the guest's UNSCALED extent while the caller
        // works in scaled coordinates, so a scaled pass has to scale here or the
        // whole pass gets clamped to the 1x extent.
        const u32 view_width = is_rescaled ? runtime.resolution.ScaleUp(view->size.width)
                                           : view->size.width;
        const u32 view_height = is_rescaled ? runtime.resolution.ScaleUp(view->size.height)
                                            : view->size.height;
        attachment_width = (std::min)(attachment_width, view_width);
        attachment_height = (std::min)(attachment_height, view_height);
        if (view->RenderTarget() != nullptr &&
            view->RenderTarget()->textureType() == MTL::TextureType3D) {
            layers = (std::max)(layers, view->RenderTargetLayers());
        } else {
            layers = (std::max)(layers, static_cast<u32>(view->range.extent.layers));
        }
    };

    u32 num_color = 0;
    color_alias_mask = 0;
    color_alias_groups = {};
    // Parent image and true subresource range per colour slot, for the overlap test below.
    // Comparing pass_state.color[].level/slice is wrong for 2D/2DArray/Cube views because
    // RenderTargetLevel/Slice are 0 (baked into the MTLTexture view), so distinct mips or
    // layers of the same parent image falsely looked identical.
    std::array<MTL::Texture*, NUM_RT> attachment_parent{};
    std::array<VideoCommon::SubresourceRange, NUM_RT> attachment_slot_ranges{};
    const auto ranges_overlap = [](const VideoCommon::SubresourceRange& a,
                                   const VideoCommon::SubresourceRange& b) {
        const bool levels = a.base.level < b.base.level + b.extent.levels &&
                            b.base.level < a.base.level + a.extent.levels;
        const bool layers = a.base.layer < b.base.layer + b.extent.layers &&
                            b.base.layer < a.base.layer + a.extent.layers;
        return levels && layers;
    };
    for (u32 index = 0; index < NUM_RT; ++index) {
        ImageView* const view = color_buffers[index];
        if (view == nullptr) {
            continue;
        }
        MTL::Texture* const render_target = view->RenderTarget();
        if (render_target == nullptr) {
            // A view we couldn't build. It MUST stay out of the count, a pass
            // that declares a colour attachment and binds nothing at that index
            // aborts the process from inside MTLCompilerService, with no
            // validation message and no error return.
            //
            // We tried substituting a scratch attachment here so the pass stays
            // complete and reverted it. Letting those draws run again is exactly
            // what the incomplete flag is for, un-guarded the mismatched writes
            // scribble tile memory into the open scene pass and Z-A came back
            // with full-screen vertical striping. That's also why
            // GetScratchAttachment has no callers. Leave it unless a Z-A run
            // says otherwise.
            static std::atomic<u32> reported{0};
            if (reported++ < 4) {
                LOG_WARNING(Render_Metal,
                            "Colour attachment {} has no usable view; leaving it unbound", index);
            }
            pass_state.incomplete = true;
            continue;
        }
        // The same texture MAY show up twice in one pass. The guest can point
        // two render target registers at one surface and we bind both instead of
        // sending one to scratch, MoltenVK does the same on this hardware and
        // renders those passes correctly.
        //
        // Comparing view pointers isn't enough, two attachments can be different
        // MTLTexture objects viewing one image and Metal's validation checks the
        // memory. Compare the parent image plus the level/slice the view renders
        // into.
        MTL::Texture* const parent_image = view->ImageHandle();

        bool aliases_earlier = false;
        u32 alias_bits = 0;
        for (u32 earlier = 0; earlier < index; ++earlier) {
            const bool same_view = pass_state.color[earlier].texture == render_target;
            const bool same_subresource =
                parent_image != nullptr && attachment_parent[earlier] == parent_image &&
                ranges_overlap(attachment_slot_ranges[earlier], view->range);
            if (!same_view && !same_subresource) {
                continue;
            }
            aliases_earlier = true;
            alias_bits = (1u << earlier) | (1u << index);
            static std::atomic<u32> reported{0};
            if (reported++ < 4) {
                LOG_DEBUG(Render_Metal,
                          "Colour attachment {} aliases {} ({}x{} fmt={}, {}); both are bound",
                          index, earlier, static_cast<u32>(render_target->width()),
                          static_cast<u32>(render_target->height()),
                          static_cast<u32>(render_target->pixelFormat()),
                          same_view ? "same view" : "same image subresource");
            }
            break;
        }
        // The validation layer aborts on overlapping attachments, which makes it
        // unusable on any title that aliases render targets. Under validation
        // only we drop the duplicate so the run continues. That differs from a
        // normal run and loses that slot's writes, so we say so in the log. Only
        // a TRAILING duplicate can go. A hole in the middle declares an
        // attachment and binds nothing, which aborts inside MTLCompilerService
        // with no message at all.
        if (aliases_earlier && ValidationLayerActive()) {
            const bool is_trailing = [&] {
                for (u32 later = index + 1; later < NUM_RT; ++later) {
                    if (color_buffers[later] != nullptr &&
                        color_buffers[later]->RenderTarget() != nullptr) {
                        return false;
                    }
                }
                return true;
            }();
            if (is_trailing) {
                static std::atomic<u32> reported{0};
                if (reported++ < 4) {
                    LOG_WARNING(Render_Metal,
                                "Validation layer active: dropping aliased colour attachment {} "
                                "so the render pass validates. Rendering WILL differ from a "
                                "normal run - writes to that slot are lost",
                                index);
                }
                continue;
            }
        }
        pass_state.color[index].texture = render_target;
        color_formats[index] = render_target->pixelFormat();
        color_alias_mask |= alias_bits;
        if (alias_bits != 0) {
            // Slots of one group all share the mask of the group.
            const u32 group = alias_bits | color_alias_groups[std::countr_zero(alias_bits)];
            for (u32 bits = group; bits != 0; bits &= bits - 1) {
                color_alias_groups[std::countr_zero(bits)] = group;
            }
        }
        attachment_parent[index] = parent_image;
        attachment_slot_ranges[index] = view->range;
        if (parent_image != nullptr) {
            attachment_ranges[num_attachment_images] = view->range;
            attachment_bits[num_attachment_images] = 1u << index;
            attachment_images[num_attachment_images++] = parent_image;
        }
        // Zero except when the target is a whole 3D texture, see
        // ImageView::RenderTargetLevel.
        pass_state.color[index].level = view->RenderTargetLevel();
        pass_state.color[index].slice = view->RenderTargetSlice();
        // Default to preserving contents; the rasterizer overrides this to
        // Clear or DontCare per draw, which is where the tile-memory win is.
        pass_state.color[index].load_action = LoadAction::Load;
        pass_state.color[index].store_action = StoreAction::Store;
        num_color = index + 1;
        pass_state.sample_count = view->Samples();
        fit(view);
    }
    pass_state.num_color = num_color;

    if (depth_buffer != nullptr) {
        pass_state.depth_stencil.texture = depth_buffer->RenderTarget();
        pass_state.depth_stencil.level = depth_buffer->RenderTargetLevel();
        pass_state.depth_stencil.slice = depth_buffer->RenderTargetSlice();
        if (pass_state.depth_stencil.texture == nullptr) {
            // Same contract as the colour slots above: a declared depth target
            // whose view failed makes the pass narrower than its pipelines.
            pass_state.incomplete = true;
        }
        if (depth_buffer->ImageHandle() != nullptr) {
            attachment_ranges[num_attachment_images] = depth_buffer->range;
            attachment_bits[num_attachment_images] = Scheduler::DEPTH_ATTACHMENT_BIT;
            attachment_images[num_attachment_images++] = depth_buffer->ImageHandle();
        }
        pass_state.depth_stencil.has_stencil =
            depth_buffer->Format() == MTL::PixelFormatDepth32Float_Stencil8 ||
            depth_buffer->Format() == MTL::PixelFormatDepth24Unorm_Stencil8 ||
            depth_buffer->Format() == MTL::PixelFormatStencil8;
        pass_state.sample_count = depth_buffer->Samples();
        fit(depth_buffer);
    }

    pass_state.num_layers = layers;

    // TIR (Target-Independent Rasterization): depth has multiple samples while every color
    // target is single-sample (e.g. Civilization VII). Metal rejects mixed-sample render
    // passes, so render into a persistent per-target MSAA color scratch and resolve back to
    // the 1x color target via StoreAction::Resolve (StoreAndMultisampleResolve).
    const bool mixed_samples =
        depth_buffer != nullptr && depth_buffer->Samples() > 1 && num_color > 0 &&
        std::ranges::all_of(color_buffers, [](const ImageView* color) {
            return color == nullptr || color->Samples() == 1;
        });
    if (mixed_samples) {
        const u32 msaa_samples = depth_buffer->Samples();
        std::array<MTL::Texture*, VideoCommon::NUM_RT> tir_scratches{};
        bool all_scratches_ok = true;
        for (u32 index = 0; index < num_color; ++index) {
            ImageView* const view = color_buffers[index];
            MTL::Texture* const single_target = pass_state.color[index].texture;
            if (view == nullptr || single_target == nullptr) {
                continue;
            }
            const u32 target_level = pass_state.color[index].level;
            const u32 target_w =
                (std::max)(1u, static_cast<u32>(single_target->width()) >> target_level);
            const u32 target_h =
                (std::max)(1u, static_cast<u32>(single_target->height()) >> target_level);
            MTL::Texture* const scratch = runtime.GetOrCreateTirColorScratch(
                view->ImageHandle() != nullptr ? view->ImageHandle() : single_target,
                single_target->pixelFormat(), target_w, target_h, layers, msaa_samples);
            if (scratch == nullptr) {
                all_scratches_ok = false;
                break;
            }
            tir_scratches[index] = scratch;
        }
        if (all_scratches_ok) {
            for (u32 index = 0; index < num_color; ++index) {
                MTL::Texture* const scratch = tir_scratches[index];
                if (scratch == nullptr) {
                    continue;
                }
                MTL::Texture* const single_target = pass_state.color[index].texture;
                pass_state.color[index].resolve_texture = single_target;
                pass_state.color[index].resolve_level = pass_state.color[index].level;
                pass_state.color[index].resolve_slice = pass_state.color[index].slice;
                pass_state.color[index].texture = scratch;
                pass_state.color[index].level = 0;
                pass_state.color[index].slice = 0;
                pass_state.color[index].load_action = LoadAction::Load;
                pass_state.color[index].store_action = StoreAction::Resolve;
            }
            pass_state.sample_count = msaa_samples;
        }
    }

}

} // namespace Metal
