// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_common/logging/log.h"
#include "yuzu_video_core/metal_common/metal_device.h"
#include "yuzu_video_core/renderer_metal/maxwell_to_mtl.h"

namespace Metal {

using VideoCore::Surface::PixelFormat;

namespace {

constexpr FormatInfo Native(MTL::PixelFormat format, bool attachable = false,
                            bool storage = false) {
    return FormatInfo{format, FormatSupport::Native, SwizzleFix::None, attachable, storage};
}

constexpr FormatInfo Swizzled(MTL::PixelFormat format, SwizzleFix fix,
                              bool attachable = false) {
    return FormatInfo{format, FormatSupport::Swizzled, fix, attachable, false};
}

constexpr FormatInfo Promoted(MTL::PixelFormat format, bool attachable = false,
                              bool storage = false) {
    return FormatInfo{format, FormatSupport::Promoted, SwizzleFix::None, attachable, storage};
}

constexpr FormatInfo Unsupported() {
    return FormatInfo{MTL::PixelFormatInvalid, FormatSupport::Unsupported, SwizzleFix::None,
                      false, false};
}

FormatInfo BaseSurfaceFormat(PixelFormat pixel_format) {
    switch (pixel_format) {
    // 8-bit per channel, 4 channels. A8B8G8R8_* is packed with alpha in the
    // high bits, which on a little-endian host is R,G,B,A in memory order.
    case PixelFormat::A8B8G8R8_UNORM:
        return Native(MTL::PixelFormatRGBA8Unorm, true, true);
    case PixelFormat::A8B8G8R8_SNORM:
        return Native(MTL::PixelFormatRGBA8Snorm, true, true);
    case PixelFormat::A8B8G8R8_SINT:
        return Native(MTL::PixelFormatRGBA8Sint, true, true);
    case PixelFormat::A8B8G8R8_UINT:
        return Native(MTL::PixelFormatRGBA8Uint, true, true);
    case PixelFormat::A8B8G8R8_SRGB:
        return Native(MTL::PixelFormatRGBA8Unorm_sRGB, true);
    case PixelFormat::B8G8R8A8_UNORM:
        return Native(MTL::PixelFormatBGRA8Unorm, true, true);
    case PixelFormat::B8G8R8A8_SRGB:
        return Native(MTL::PixelFormatBGRA8Unorm_sRGB, true);

    // Packed 16-bit colour. By the LSB-up rule above, MTLPixelFormatB5G6R5Unorm
    // is the equivalent of VK_FORMAT_R5G6B5_UNORM_PACK16, not of the identically
    // spelled VK_FORMAT_B5G6R5_UNORM_PACK16.
    //
    // A swizzle repairs reads, not writes, so a Swizzled format is never a valid
    // attachment: a SwapRedBlue render target would store the shader's red into
    // blue. Only the Native entry may be attachable.
    case PixelFormat::R5G6B5_UNORM:
        return Native(MTL::PixelFormatB5G6R5Unorm, true);
    case PixelFormat::B5G6R5_UNORM:
        return Swizzled(MTL::PixelFormatB5G6R5Unorm, SwizzleFix::SwapRedBlue);
    // Same convention flip: MTLPixelFormatBGR5A1Unorm has alpha in the HIGH bits
    // (VK_FORMAT_A1R5G5B5_UNORM_PACK16), MTLPixelFormatA1BGR5Unorm in the LOW
    // bit (VK_FORMAT_R5G5B5A1_UNORM_PACK16). Getting these two the wrong way
    // round moves the 1-bit alpha to the other end of the texel, which no
    // component swizzle can repair.
    case PixelFormat::A1R5G5B5_UNORM:
        return Native(MTL::PixelFormatBGR5A1Unorm, true);
    case PixelFormat::A1B5G5R5_UNORM:
        return Swizzled(MTL::PixelFormatBGR5A1Unorm, SwizzleFix::SwapRedBlue);
    // A5B5G5R1 against A1BGR5 needs A<->R and G<->B, which the view applies as
    // SwizzleFix::SwapSpecial.
    case PixelFormat::A5B5G5R1_UNORM:
        return Swizzled(MTL::PixelFormatA1BGR5Unorm, SwizzleFix::SwapSpecial);
    // MTLPixelFormatABGR4Unorm holds alpha in the LOW nibble
    // (VK_FORMAT_R4G4B4A4_UNORM_PACK16), the exact reverse of the guest layout,
    // so the view reverses all four channels.
    case PixelFormat::A4B4G4R4_UNORM:
        return Swizzled(MTL::PixelFormatABGR4Unorm, SwizzleFix::Reverse);

    // Packed 32-bit colour.
    case PixelFormat::A2B10G10R10_UNORM:
        return Native(MTL::PixelFormatRGB10A2Unorm, true, true);
    case PixelFormat::A2B10G10R10_UINT:
        return Native(MTL::PixelFormatRGB10A2Uint, true, true);
    case PixelFormat::A2R10G10B10_UNORM:
        return Native(MTL::PixelFormatBGR10A2Unorm, true);
    case PixelFormat::B10G11R11_FLOAT:
        return Native(MTL::PixelFormatRG11B10Float, true, true);
    case PixelFormat::E5B9G9R9_FLOAT:
        return Native(MTL::PixelFormatRGB9E5Float);

    // Single channel.
    case PixelFormat::R8_UNORM:
        return Native(MTL::PixelFormatR8Unorm, true, true);
    case PixelFormat::R8_SNORM:
        return Native(MTL::PixelFormatR8Snorm, true, true);
    case PixelFormat::R8_SINT:
        return Native(MTL::PixelFormatR8Sint, true, true);
    case PixelFormat::R8_UINT:
        return Native(MTL::PixelFormatR8Uint, true, true);
    case PixelFormat::R16_FLOAT:
        return Native(MTL::PixelFormatR16Float, true, true);
    case PixelFormat::R16_UNORM:
        return Native(MTL::PixelFormatR16Unorm, true, true);
    case PixelFormat::R16_SNORM:
        return Native(MTL::PixelFormatR16Snorm, true, true);
    case PixelFormat::R16_UINT:
        return Native(MTL::PixelFormatR16Uint, true, true);
    case PixelFormat::R16_SINT:
        return Native(MTL::PixelFormatR16Sint, true, true);
    case PixelFormat::R32_FLOAT:
        return Native(MTL::PixelFormatR32Float, true, true);
    case PixelFormat::R32_UINT:
        return Native(MTL::PixelFormatR32Uint, true, true);
    case PixelFormat::R32_SINT:
        return Native(MTL::PixelFormatR32Sint, true, true);

    // Two channels.
    case PixelFormat::R8G8_UNORM:
        return Native(MTL::PixelFormatRG8Unorm, true, true);
    case PixelFormat::R8G8_SNORM:
        return Native(MTL::PixelFormatRG8Snorm, true, true);
    case PixelFormat::R8G8_SINT:
        return Native(MTL::PixelFormatRG8Sint, true, true);
    case PixelFormat::R8G8_UINT:
        return Native(MTL::PixelFormatRG8Uint, true, true);
    case PixelFormat::R16G16_UNORM:
        return Native(MTL::PixelFormatRG16Unorm, true, true);
    case PixelFormat::R16G16_SNORM:
        return Native(MTL::PixelFormatRG16Snorm, true, true);
    case PixelFormat::R16G16_FLOAT:
        return Native(MTL::PixelFormatRG16Float, true, true);
    case PixelFormat::R16G16_UINT:
        return Native(MTL::PixelFormatRG16Uint, true, true);
    case PixelFormat::R16G16_SINT:
        return Native(MTL::PixelFormatRG16Sint, true, true);
    case PixelFormat::R32G32_FLOAT:
        return Native(MTL::PixelFormatRG32Float, true, true);
    case PixelFormat::R32G32_UINT:
        return Native(MTL::PixelFormatRG32Uint, true, true);
    case PixelFormat::R32G32_SINT:
        return Native(MTL::PixelFormatRG32Sint, true, true);

    // Four channels, wide.
    case PixelFormat::R16G16B16A16_FLOAT:
        return Native(MTL::PixelFormatRGBA16Float, true, true);
    case PixelFormat::R16G16B16A16_UNORM:
        return Native(MTL::PixelFormatRGBA16Unorm, true, true);
    case PixelFormat::R16G16B16A16_SNORM:
        return Native(MTL::PixelFormatRGBA16Snorm, true, true);
    case PixelFormat::R16G16B16A16_UINT:
        return Native(MTL::PixelFormatRGBA16Uint, true, true);
    case PixelFormat::R16G16B16A16_SINT:
        return Native(MTL::PixelFormatRGBA16Sint, true, true);
    case PixelFormat::R16G16B16X16_FLOAT:
        // No X variant in Metal; alpha is present but ignored by the guest.
        // Storage-capable for the same reason R16G16B16A16_FLOAT above is: it
        // IS that Metal format. Leaving it off dropped the shader-write usage
        // and with it any compute path that writes this surface.
        return Promoted(MTL::PixelFormatRGBA16Float, true, true);
    case PixelFormat::R32G32B32A32_FLOAT:
        return Native(MTL::PixelFormatRGBA32Float, true, true);
    case PixelFormat::R32G32B32A32_UINT:
        return Native(MTL::PixelFormatRGBA32Uint, true, true);
    case PixelFormat::R32G32B32A32_SINT:
        return Native(MTL::PixelFormatRGBA32Sint, true, true);

    case PixelFormat::R32G32B32_FLOAT:
        return Promoted(MTL::PixelFormatRGBA32Float);
    case PixelFormat::G4R4_UNORM:
        return Promoted(MTL::PixelFormatRG8Unorm);

    // BC / DXT.
    case PixelFormat::BC1_RGBA_UNORM:
        return Native(MTL::PixelFormatBC1_RGBA);
    case PixelFormat::BC1_RGBA_SRGB:
        return Native(MTL::PixelFormatBC1_RGBA_sRGB);
    case PixelFormat::BC2_UNORM:
        return Native(MTL::PixelFormatBC2_RGBA);
    case PixelFormat::BC2_SRGB:
        return Native(MTL::PixelFormatBC2_RGBA_sRGB);
    case PixelFormat::BC3_UNORM:
        return Native(MTL::PixelFormatBC3_RGBA);
    case PixelFormat::BC3_SRGB:
        return Native(MTL::PixelFormatBC3_RGBA_sRGB);
    case PixelFormat::BC4_UNORM:
        return Native(MTL::PixelFormatBC4_RUnorm);
    case PixelFormat::BC4_SNORM:
        return Native(MTL::PixelFormatBC4_RSnorm);
    case PixelFormat::BC5_UNORM:
        return Native(MTL::PixelFormatBC5_RGUnorm);
    case PixelFormat::BC5_SNORM:
        return Native(MTL::PixelFormatBC5_RGSnorm);
    case PixelFormat::BC6H_UFLOAT:
        return Native(MTL::PixelFormatBC6H_RGBUfloat);
    case PixelFormat::BC6H_SFLOAT:
        return Native(MTL::PixelFormatBC6H_RGBFloat);
    case PixelFormat::BC7_UNORM:
        return Native(MTL::PixelFormatBC7_RGBAUnorm);
    case PixelFormat::BC7_SRGB:
        return Native(MTL::PixelFormatBC7_RGBAUnorm_sRGB);

    // ASTC.
    case PixelFormat::ASTC_2D_4X4_UNORM:
        return Native(MTL::PixelFormatASTC_4x4_LDR);
    case PixelFormat::ASTC_2D_4X4_SRGB:
        return Native(MTL::PixelFormatASTC_4x4_sRGB);
    case PixelFormat::ASTC_2D_5X4_UNORM:
        return Native(MTL::PixelFormatASTC_5x4_LDR);
    case PixelFormat::ASTC_2D_5X4_SRGB:
        return Native(MTL::PixelFormatASTC_5x4_sRGB);
    case PixelFormat::ASTC_2D_5X5_UNORM:
        return Native(MTL::PixelFormatASTC_5x5_LDR);
    case PixelFormat::ASTC_2D_5X5_SRGB:
        return Native(MTL::PixelFormatASTC_5x5_sRGB);
    case PixelFormat::ASTC_2D_6X5_UNORM:
        return Native(MTL::PixelFormatASTC_6x5_LDR);
    case PixelFormat::ASTC_2D_6X5_SRGB:
        return Native(MTL::PixelFormatASTC_6x5_sRGB);
    case PixelFormat::ASTC_2D_6X6_UNORM:
        return Native(MTL::PixelFormatASTC_6x6_LDR);
    case PixelFormat::ASTC_2D_6X6_SRGB:
        return Native(MTL::PixelFormatASTC_6x6_sRGB);
    case PixelFormat::ASTC_2D_8X5_UNORM:
        return Native(MTL::PixelFormatASTC_8x5_LDR);
    case PixelFormat::ASTC_2D_8X5_SRGB:
        return Native(MTL::PixelFormatASTC_8x5_sRGB);
    case PixelFormat::ASTC_2D_8X6_UNORM:
        return Native(MTL::PixelFormatASTC_8x6_LDR);
    case PixelFormat::ASTC_2D_8X6_SRGB:
        return Native(MTL::PixelFormatASTC_8x6_sRGB);
    case PixelFormat::ASTC_2D_8X8_UNORM:
        return Native(MTL::PixelFormatASTC_8x8_LDR);
    case PixelFormat::ASTC_2D_8X8_SRGB:
        return Native(MTL::PixelFormatASTC_8x8_sRGB);
    case PixelFormat::ASTC_2D_10X5_UNORM:
        return Native(MTL::PixelFormatASTC_10x5_LDR);
    case PixelFormat::ASTC_2D_10X5_SRGB:
        return Native(MTL::PixelFormatASTC_10x5_sRGB);
    case PixelFormat::ASTC_2D_10X6_UNORM:
        return Native(MTL::PixelFormatASTC_10x6_LDR);
    case PixelFormat::ASTC_2D_10X6_SRGB:
        return Native(MTL::PixelFormatASTC_10x6_sRGB);
    case PixelFormat::ASTC_2D_10X8_UNORM:
        return Native(MTL::PixelFormatASTC_10x8_LDR);
    case PixelFormat::ASTC_2D_10X8_SRGB:
        return Native(MTL::PixelFormatASTC_10x8_sRGB);
    case PixelFormat::ASTC_2D_10X10_UNORM:
        return Native(MTL::PixelFormatASTC_10x10_LDR);
    case PixelFormat::ASTC_2D_10X10_SRGB:
        return Native(MTL::PixelFormatASTC_10x10_sRGB);
    case PixelFormat::ASTC_2D_12X10_UNORM:
        return Native(MTL::PixelFormatASTC_12x10_LDR);
    case PixelFormat::ASTC_2D_12X10_SRGB:
        return Native(MTL::PixelFormatASTC_12x10_sRGB);
    case PixelFormat::ASTC_2D_12X12_UNORM:
        return Native(MTL::PixelFormatASTC_12x12_LDR);
    case PixelFormat::ASTC_2D_12X12_SRGB:
        return Native(MTL::PixelFormatASTC_12x12_sRGB);


    // Depth and stencil.
    case PixelFormat::D32_FLOAT:
        return Native(MTL::PixelFormatDepth32Float, true);
    case PixelFormat::D16_UNORM:
        return Native(MTL::PixelFormatDepth16Unorm, true);
    case PixelFormat::S8_UINT:
        return Native(MTL::PixelFormatStencil8, true);
    case PixelFormat::X8_D24_UNORM:
        // No 24-bit-only depth in Metal. Widening to 32-bit float depth is
        // safe: it strictly increases precision over 24-bit unorm across the
        // 0..1 range the guest uses.
        return Promoted(MTL::PixelFormatDepth32Float, true);
    case PixelFormat::D24_UNORM_S8_UINT:
    case PixelFormat::S8_UINT_D24_UNORM:
        // Resolved against the device below, Apple GPUs have no D24S8.
        return Native(MTL::PixelFormatDepth24Unorm_Stencil8, true);
    case PixelFormat::D32_FLOAT_S8_UINT:
        return Native(MTL::PixelFormatDepth32Float_Stencil8, true);

    default:
        return Unsupported();
    }
}

/// Compressed families are wholly absent on some of the hardware we support,
/// not just slow, so they have to be gated instead of assumed.
bool IsCompressedFamilySupported(const Device& device, PixelFormat pixel_format) {
    using namespace VideoCore::Surface;

    if (IsPixelFormatBCn(pixel_format)) {
        return device.SupportsBCTextureCompression();
    }
    if (IsPixelFormatASTC(pixel_format)) {
        // ASTC and ETC2 are Apple-GPU features. An Intel Mac has neither, and
        // the texture cache must decompress on the CPU instead.
        return device.IsTileBased();
    }
    return true;
}

} // Anonymous namespace

bool IsCompressedFamilyNative(const Device& device,
                              VideoCore::Surface::PixelFormat pixel_format) {
    return IsCompressedFamilySupported(device, pixel_format);
}

FormatInfo SurfaceFormat(const Device& device, PixelFormat pixel_format) {
    FormatInfo info = BaseSurfaceFormat(pixel_format);

    if (info.support == FormatSupport::Unsupported) {
        return info;
    }

    // A compressed family the device lacks is DECOMPRESSED, not dropped. The
    // shared cache decodes on the CPU into the uncompressed format named here,
    // with Image::Image setting ImageFlagBits::Converted to route it. Returning
    // Unsupported would leave the texture null instead.
    if (!IsCompressedFamilySupported(device, pixel_format)) {
        info.format = VideoCore::Surface::IsPixelFormatSRGB(pixel_format)
                          ? MTL::PixelFormatRGBA8Unorm_sRGB
                          : MTL::PixelFormatRGBA8Unorm;
        info.support = FormatSupport::Promoted;
        info.swizzle_fix = SwizzleFix::None;
        info.attachable = false;
        info.storage = false;
        return info;
    }

    // D24S8 does not exist on Apple GPUs. Promote to D32S8 rather than failing:
    // it costs 4 bytes per pixel more but is otherwise a superset, and depth
    // precision only improves.
    if (info.format == MTL::PixelFormatDepth24Unorm_Stencil8 &&
        !device.SupportsDepth24Stencil8()) {
        info.format = MTL::PixelFormatDepth32Float_Stencil8;
        info.support = FormatSupport::Promoted;
    }

    return info;
}

MTL::VertexFormat VertexFormat(Tegra::Engines::Maxwell3D::Regs::VertexAttribute::Size size,
                               Tegra::Engines::Maxwell3D::Regs::VertexAttribute::Type type) {
    using Size = Tegra::Engines::Maxwell3D::Regs::VertexAttribute::Size;
    using Type = Tegra::Engines::Maxwell3D::Regs::VertexAttribute::Type;

    switch (size) {
    case Size::Size_R8:
        switch (type) {
        case Type::UNorm: return MTL::VertexFormatUCharNormalized;
        case Type::SNorm: return MTL::VertexFormatCharNormalized;
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUChar;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatChar;
        default: break;
        }
        break;
    case Size::Size_R8_G8:
    case Size::Size_G8_R8:
        switch (type) {
        case Type::UNorm: return MTL::VertexFormatUChar2Normalized;
        case Type::SNorm: return MTL::VertexFormatChar2Normalized;
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUChar2;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatChar2;
        default: break;
        }
        break;
    case Size::Size_R8_G8_B8:
        switch (type) {
        case Type::UNorm: return MTL::VertexFormatUChar3Normalized;
        case Type::SNorm: return MTL::VertexFormatChar3Normalized;
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUChar3;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatChar3;
        default: break;
        }
        break;
    case Size::Size_R8_G8_B8_A8:
    case Size::Size_X8_B8_G8_R8:
        switch (type) {
        case Type::UNorm: return MTL::VertexFormatUChar4Normalized;
        case Type::SNorm: return MTL::VertexFormatChar4Normalized;
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUChar4;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatChar4;
        default: break;
        }
        break;
    case Size::Size_R16:
        switch (type) {
        case Type::UNorm: return MTL::VertexFormatUShortNormalized;
        case Type::SNorm: return MTL::VertexFormatShortNormalized;
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUShort;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatShort;
        case Type::Float: return MTL::VertexFormatHalf;
        default: break;
        }
        break;
    case Size::Size_R16_G16:
        switch (type) {
        case Type::UNorm: return MTL::VertexFormatUShort2Normalized;
        case Type::SNorm: return MTL::VertexFormatShort2Normalized;
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUShort2;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatShort2;
        case Type::Float: return MTL::VertexFormatHalf2;
        default: break;
        }
        break;
    case Size::Size_R16_G16_B16:
        switch (type) {
        case Type::UNorm: return MTL::VertexFormatUShort3Normalized;
        case Type::SNorm: return MTL::VertexFormatShort3Normalized;
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUShort3;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatShort3;
        case Type::Float: return MTL::VertexFormatHalf3;
        default: break;
        }
        break;
    case Size::Size_R16_G16_B16_A16:
        switch (type) {
        case Type::UNorm: return MTL::VertexFormatUShort4Normalized;
        case Type::SNorm: return MTL::VertexFormatShort4Normalized;
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUShort4;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatShort4;
        case Type::Float: return MTL::VertexFormatHalf4;
        default: break;
        }
        break;
    case Size::Size_R32:
        switch (type) {
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUInt;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatInt;
        case Type::Float: return MTL::VertexFormatFloat;
        default: break;
        }
        break;
    case Size::Size_R32_G32:
        switch (type) {
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUInt2;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatInt2;
        case Type::Float: return MTL::VertexFormatFloat2;
        default: break;
        }
        break;
    case Size::Size_R32_G32_B32:
        switch (type) {
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUInt3;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatInt3;
        case Type::Float: return MTL::VertexFormatFloat3;
        default: break;
        }
        break;
    case Size::Size_R32_G32_B32_A32:
        switch (type) {
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUInt4;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatInt4;
        case Type::Float: return MTL::VertexFormatFloat4;
        default: break;
        }
        break;
    case Size::Size_A2_B10_G10_R10:
        switch (type) {
        case Type::UNorm: return MTL::VertexFormatUInt1010102Normalized;
        case Type::SNorm: return MTL::VertexFormatInt1010102Normalized;
        default: break;
        }
        break;
    case Size::Size_B10_G11_R11:
        // Float only: a packed float format with no sign bit has no meaningful
        // normalised or integer reading, and Vulkan likewise handles this size
        // only in its Type::Float branch. Anything else falls through to Invalid
        // so the unsupported-attribute path reports it.
        switch (type) {
        case Type::Float: return MTL::VertexFormatFloatRG11B10;
        default: break;
        }
        break;
    case Size::Size_A8:
        // Same handling as Size_R8, one 8-bit component, and Vulkan maps the two
        // sizes to the same format in all six type branches (maxwell_to_vk.cpp).
        // Only UNorm and UInt were covered here, so an SNorm, SInt, UScaled or
        // SScaled A8 attribute fell through to Invalid and the caller dropped it.
        // See the warning below, a dropped position collapses the mesh to the
        // origin.
        switch (type) {
        case Type::UNorm: return MTL::VertexFormatUCharNormalized;
        case Type::SNorm: return MTL::VertexFormatCharNormalized;
        case Type::UInt: case Type::UScaled: return MTL::VertexFormatUChar;
        case Type::SInt: case Type::SScaled: return MTL::VertexFormatChar;
        default: break;
        }
        break;
    default:
        break;
    }

    // Worth a warning: the caller drops the attribute entirely, so a dropped
    // position collapses the mesh to the origin.
    static std::atomic<u32> reported{0};
    if (reported++ < 8) {
        LOG_WARNING(Render_Metal, "Unsupported vertex format: size {} type {}",
                    static_cast<u32>(size), static_cast<u32>(type));
    }
    return MTL::VertexFormatInvalid;
}

u32 VertexFormatSize(MTL::VertexFormat format) {
    switch (format) {
    case MTL::VertexFormatUChar:
    case MTL::VertexFormatChar:
    case MTL::VertexFormatUCharNormalized:
    case MTL::VertexFormatCharNormalized:
        return 1;
    case MTL::VertexFormatUChar2:
    case MTL::VertexFormatChar2:
    case MTL::VertexFormatUChar2Normalized:
    case MTL::VertexFormatChar2Normalized:
    case MTL::VertexFormatUShort:
    case MTL::VertexFormatShort:
    case MTL::VertexFormatUShortNormalized:
    case MTL::VertexFormatShortNormalized:
    case MTL::VertexFormatHalf:
        return 2;
    // Metal packs the three-component byte formats into three bytes, not four.
    case MTL::VertexFormatUChar3:
    case MTL::VertexFormatChar3:
    case MTL::VertexFormatUChar3Normalized:
    case MTL::VertexFormatChar3Normalized:
        return 3;
    case MTL::VertexFormatUChar4:
    case MTL::VertexFormatChar4:
    case MTL::VertexFormatUChar4Normalized:
    case MTL::VertexFormatChar4Normalized:
    case MTL::VertexFormatUShort2:
    case MTL::VertexFormatShort2:
    case MTL::VertexFormatUShort2Normalized:
    case MTL::VertexFormatShort2Normalized:
    case MTL::VertexFormatHalf2:
    case MTL::VertexFormatUInt:
    case MTL::VertexFormatInt:
    case MTL::VertexFormatFloat:
    case MTL::VertexFormatUInt1010102Normalized:
    case MTL::VertexFormatInt1010102Normalized:
    case MTL::VertexFormatFloatRG11B10:
        return 4;
    case MTL::VertexFormatUShort3:
    case MTL::VertexFormatShort3:
    case MTL::VertexFormatUShort3Normalized:
    case MTL::VertexFormatShort3Normalized:
    case MTL::VertexFormatHalf3:
        return 6;
    case MTL::VertexFormatUShort4:
    case MTL::VertexFormatShort4:
    case MTL::VertexFormatUShort4Normalized:
    case MTL::VertexFormatShort4Normalized:
    case MTL::VertexFormatHalf4:
    case MTL::VertexFormatUInt2:
    case MTL::VertexFormatInt2:
    case MTL::VertexFormatFloat2:
        return 8;
    case MTL::VertexFormatUInt3:
    case MTL::VertexFormatInt3:
    case MTL::VertexFormatFloat3:
        return 12;
    case MTL::VertexFormatUInt4:
    case MTL::VertexFormatInt4:
    case MTL::VertexFormatFloat4:
        return 16;
    default:
        return 0;
    }
}

MTL::CompareFunction ComparisonOp(Tegra::Engines::Maxwell3D::Regs::ComparisonOp comparison) {
    using Op = Tegra::Engines::Maxwell3D::Regs::ComparisonOp;
    switch (comparison) {
    case Op::Never_D3D:
    case Op::Never_GL:
        return MTL::CompareFunctionNever;
    case Op::Less_D3D:
    case Op::Less_GL:
        return MTL::CompareFunctionLess;
    case Op::Equal_D3D:
    case Op::Equal_GL:
        return MTL::CompareFunctionEqual;
    case Op::LessEqual_D3D:
    case Op::LessEqual_GL:
        return MTL::CompareFunctionLessEqual;
    case Op::Greater_D3D:
    case Op::Greater_GL:
        return MTL::CompareFunctionGreater;
    case Op::NotEqual_D3D:
    case Op::NotEqual_GL:
        return MTL::CompareFunctionNotEqual;
    case Op::GreaterEqual_D3D:
    case Op::GreaterEqual_GL:
        return MTL::CompareFunctionGreaterEqual;
    case Op::Always_D3D:
    case Op::Always_GL:
        return MTL::CompareFunctionAlways;
    }
    return MTL::CompareFunctionAlways;
}

MTL::CompareFunction DepthCompareFunction(Tegra::Texture::DepthCompareFunc func) {
    using Func = Tegra::Texture::DepthCompareFunc;
    switch (func) {
    case Func::Never:
        return MTL::CompareFunctionNever;
    case Func::Less:
        return MTL::CompareFunctionLess;
    case Func::Equal:
        return MTL::CompareFunctionEqual;
    case Func::LessEqual:
        return MTL::CompareFunctionLessEqual;
    case Func::Greater:
        return MTL::CompareFunctionGreater;
    case Func::NotEqual:
        return MTL::CompareFunctionNotEqual;
    case Func::GreaterEqual:
        return MTL::CompareFunctionGreaterEqual;
    case Func::Always:
        return MTL::CompareFunctionAlways;
    }
    return MTL::CompareFunctionAlways;
}

MTL::SamplerMinMagFilter SamplerFilter(Tegra::Texture::TextureFilter filter) {
    switch (filter) {
    case Tegra::Texture::TextureFilter::Nearest:
        return MTL::SamplerMinMagFilterNearest;
    case Tegra::Texture::TextureFilter::Linear:
        return MTL::SamplerMinMagFilterLinear;
    }
    return MTL::SamplerMinMagFilterLinear;
}

MTL::SamplerMipFilter SamplerMipFilter(Tegra::Texture::TextureMipmapFilter filter) {
    switch (filter) {
    case Tegra::Texture::TextureMipmapFilter::None:
        return MTL::SamplerMipFilterNotMipmapped;
    case Tegra::Texture::TextureMipmapFilter::Nearest:
        return MTL::SamplerMipFilterNearest;
    case Tegra::Texture::TextureMipmapFilter::Linear:
        return MTL::SamplerMipFilterLinear;
    }
    return MTL::SamplerMipFilterLinear;
}

MTL::SamplerReductionMode SamplerReductionMode(Tegra::Texture::SamplerReduction reduction) {
    // One-to-one: Metal offers the same three modes on the plain sampler
    // descriptor, where Vulkan needs VK_EXT_sampler_filter_minmax.
    switch (reduction) {
    case Tegra::Texture::SamplerReduction::WeightedAverage:
        return MTL::SamplerReductionModeWeightedAverage;
    case Tegra::Texture::SamplerReduction::Min:
        return MTL::SamplerReductionModeMinimum;
    case Tegra::Texture::SamplerReduction::Max:
        return MTL::SamplerReductionModeMaximum;
    }
    return MTL::SamplerReductionModeWeightedAverage;
}

MTL::SamplerAddressMode WrapMode(const Device& device, Tegra::Texture::WrapMode wrap_mode,
                                 Tegra::Texture::TextureFilter filter) {
    switch (wrap_mode) {
    case Tegra::Texture::WrapMode::Wrap:
        return MTL::SamplerAddressModeRepeat;
    case Tegra::Texture::WrapMode::Mirror:
        return MTL::SamplerAddressModeMirrorRepeat;
    case Tegra::Texture::WrapMode::ClampToEdge:
        return MTL::SamplerAddressModeClampToEdge;
    case Tegra::Texture::WrapMode::Border:
        return MTL::SamplerAddressModeClampToBorderColor;
    case Tegra::Texture::WrapMode::Clamp:
        // Maxwell's Clamp is GL_CLAMP, which resolves differently per filter:
        // nearest takes the edge texel, linear blends towards the border.
        return filter == Tegra::Texture::TextureFilter::Nearest
                   ? MTL::SamplerAddressModeClampToEdge
                   : MTL::SamplerAddressModeClampToBorderColor;
    case Tegra::Texture::WrapMode::MirrorOnceClampToEdge:
        return MTL::SamplerAddressModeMirrorClampToEdge;
    case Tegra::Texture::WrapMode::MirrorOnceBorder:
        // Metal has no mirror-once-to-border. Edge is the closest behaviour
        // and matches what the Vulkan backend falls back to on drivers lacking
        // VK_KHR_sampler_mirror_clamp_to_edge.
        return MTL::SamplerAddressModeMirrorClampToEdge;
    case Tegra::Texture::WrapMode::MirrorOnceClampOGL:
        return MTL::SamplerAddressModeMirrorClampToEdge;
    }
    return MTL::SamplerAddressModeClampToEdge;
}

MTL::BlendFactor BlendFactor(Tegra::Engines::Maxwell3D::Regs::Blend::Factor factor) {
    using Factor = Tegra::Engines::Maxwell3D::Regs::Blend::Factor;
    switch (factor) {
    case Factor::Zero_D3D:
    case Factor::Zero_GL:
        return MTL::BlendFactorZero;
    case Factor::One_D3D:
    case Factor::One_GL:
        return MTL::BlendFactorOne;
    case Factor::SourceColor_D3D:
    case Factor::SourceColor_GL:
        return MTL::BlendFactorSourceColor;
    case Factor::OneMinusSourceColor_D3D:
    case Factor::OneMinusSourceColor_GL:
        return MTL::BlendFactorOneMinusSourceColor;
    case Factor::SourceAlpha_D3D:
    case Factor::SourceAlpha_GL:
        return MTL::BlendFactorSourceAlpha;
    case Factor::OneMinusSourceAlpha_D3D:
    case Factor::OneMinusSourceAlpha_GL:
        return MTL::BlendFactorOneMinusSourceAlpha;
    case Factor::DestAlpha_D3D:
    case Factor::DestAlpha_GL:
        return MTL::BlendFactorDestinationAlpha;
    case Factor::OneMinusDestAlpha_D3D:
    case Factor::OneMinusDestAlpha_GL:
        return MTL::BlendFactorOneMinusDestinationAlpha;
    case Factor::DestColor_D3D:
    case Factor::DestColor_GL:
        return MTL::BlendFactorDestinationColor;
    case Factor::OneMinusDestColor_D3D:
    case Factor::OneMinusDestColor_GL:
        return MTL::BlendFactorOneMinusDestinationColor;
    case Factor::SourceAlphaSaturate_D3D:
    case Factor::SourceAlphaSaturate_GL:
        return MTL::BlendFactorSourceAlphaSaturated;
    case Factor::Source1Color_D3D:
    case Factor::Source1Color_GL:
        return MTL::BlendFactorSource1Color;
    case Factor::OneMinusSource1Color_D3D:
    case Factor::OneMinusSource1Color_GL:
        return MTL::BlendFactorOneMinusSource1Color;
    case Factor::Source1Alpha_D3D:
    case Factor::Source1Alpha_GL:
        return MTL::BlendFactorSource1Alpha;
    case Factor::OneMinusSource1Alpha_D3D:
    case Factor::OneMinusSource1Alpha_GL:
        return MTL::BlendFactorOneMinusSource1Alpha;
    // Despite their names, these D3D spellings are not duplicates of the
    // dual-source factors above: Maxwell maps BlendFactor_D3D and
    // BothSourceAlpha_D3D onto the constant colour and constant alpha, as the
    // Vulkan backend also does.
    case Factor::BlendFactor_D3D:
    case Factor::ConstantColor_GL:
        return MTL::BlendFactorBlendColor;
    case Factor::OneMinusBlendFactor_D3D:
    case Factor::OneMinusConstantColor_GL:
        return MTL::BlendFactorOneMinusBlendColor;
    case Factor::BothSourceAlpha_D3D:
    case Factor::ConstantAlpha_GL:
        return MTL::BlendFactorBlendAlpha;
    case Factor::OneMinusBothSourceAlpha_D3D:
    case Factor::OneMinusConstantAlpha_GL:
        return MTL::BlendFactorOneMinusBlendAlpha;
    default:
        // Every known factor is handled above. Zero is the safe fallback, but it
        // does change blending, so it is worth a warning.
        LOG_WARNING(Render_Metal, "Unhandled blend factor {}", static_cast<u32>(factor));
        return MTL::BlendFactorZero;
    }
}

MTL::BlendOperation BlendEquation(Tegra::Engines::Maxwell3D::Regs::Blend::Equation equation) {
    using Equation = Tegra::Engines::Maxwell3D::Regs::Blend::Equation;
    switch (equation) {
    case Equation::Add_D3D:
    case Equation::Add_GL:
        return MTL::BlendOperationAdd;
    case Equation::Subtract_D3D:
    case Equation::Subtract_GL:
        return MTL::BlendOperationSubtract;
    case Equation::ReverseSubtract_D3D:
    case Equation::ReverseSubtract_GL:
        return MTL::BlendOperationReverseSubtract;
    case Equation::Min_D3D:
    case Equation::Min_GL:
        return MTL::BlendOperationMin;
    case Equation::Max_D3D:
    case Equation::Max_GL:
        return MTL::BlendOperationMax;
    }
    return MTL::BlendOperationAdd;
}

MTL::CullMode CullFace(Tegra::Engines::Maxwell3D::Regs::CullFace cull_face) {
    using Cull = Tegra::Engines::Maxwell3D::Regs::CullFace;
    switch (cull_face) {
    case Cull::Front:
        return MTL::CullModeFront;
    case Cull::Back:
        return MTL::CullModeBack;
    case Cull::FrontAndBack:
        // Metal has no front-and-back cull mode. The rasterizer disables
        // rasterization outright for this case instead.
        return MTL::CullModeNone;
    }
    return MTL::CullModeNone;
}

MTL::StencilOperation StencilOp(Tegra::Engines::Maxwell3D::Regs::StencilOp::Op stencil_op) {
    using Op = Tegra::Engines::Maxwell3D::Regs::StencilOp::Op;
    switch (stencil_op) {
    case Op::Keep_D3D:
    case Op::Keep_GL:
        return MTL::StencilOperationKeep;
    case Op::Zero_D3D:
    case Op::Zero_GL:
        return MTL::StencilOperationZero;
    case Op::Replace_D3D:
    case Op::Replace_GL:
        return MTL::StencilOperationReplace;
    case Op::IncrSaturate_D3D:
    case Op::IncrSaturate_GL:
        return MTL::StencilOperationIncrementClamp;
    case Op::DecrSaturate_D3D:
    case Op::DecrSaturate_GL:
        return MTL::StencilOperationDecrementClamp;
    case Op::Invert_D3D:
    case Op::Invert_GL:
        return MTL::StencilOperationInvert;
    case Op::Incr_D3D:
    case Op::Incr_GL:
        return MTL::StencilOperationIncrementWrap;
    case Op::Decr_D3D:
    case Op::Decr_GL:
        return MTL::StencilOperationDecrementWrap;
    }
    return MTL::StencilOperationKeep;
}

MTL::Winding FrontFace(Tegra::Engines::Maxwell3D::Regs::FrontFace front_face) {
    switch (front_face) {
    case Tegra::Engines::Maxwell3D::Regs::FrontFace::ClockWise:
        return MTL::WindingClockwise;
    case Tegra::Engines::Maxwell3D::Regs::FrontFace::CounterClockWise:
        return MTL::WindingCounterClockwise;
    }
    return MTL::WindingCounterClockwise;
}

MTL::PrimitiveType PrimitiveTopology(const Device& device,
                                     Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology) {
    using Topology = Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology;
    switch (topology) {
    case Topology::Points:
        return MTL::PrimitiveTypePoint;
    case Topology::Lines:
        return MTL::PrimitiveTypeLine;
    case Topology::LineStrip:
        return MTL::PrimitiveTypeLineStrip;
    case Topology::Triangles:
        return MTL::PrimitiveTypeTriangle;
    case Topology::TriangleStrip:
        return MTL::PrimitiveTypeTriangleStrip;
    default:
        // Metal has no fans, loops, adjacency primitives or quads, these get
        // rewritten into triangle or line lists before the draw.
        LOG_DEBUG(Render_Metal, "Topology {} needs index conversion",
                  static_cast<u32>(topology));
        return MTL::PrimitiveTypeTriangle;
    }
}

} // namespace Metal
