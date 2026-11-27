// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "yuzu_common/common_types.h"
#include "yuzu_video_core/engines/maxwell_3d.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/surface.h"
#include "yuzu_video_core/textures/texture.h"

namespace Metal {

class Device;

/// How a guest pixel format was satisfied on this device.
enum class FormatSupport : u8 {
    Native,     ///< Metal has the exact format.
    Promoted,   ///< Substituted for a wider format with identical semantics
                ///< (D24S8 becoming D32S8 on Apple GPUs, for instance).
    Swizzled,   ///< Layout matches after a component swizzle on the view.
    Unsupported ///< Needs CPU-side conversion before upload.
};

/// How a sampled view has to permute its components to undo a format
/// substitution. Named after the transform rather than the format so the table
/// below reads as an instruction.
enum class SwizzleFix {
    None,
    SwapRedBlue,  ///< R <-> B.
    SwapGreenRed, ///< R <-> G.
    SwapSpecial,  ///< A <-> R and G <-> B, for A5B5G5R1 against A1BGR5.
    Reverse,      ///< Full channel reversal, for A4B4G4R4 against ABGR4.
};

struct FormatInfo {
    MTL::PixelFormat format{MTL::PixelFormatInvalid};
    FormatSupport support{FormatSupport::Unsupported};
    /// Component permutation a sampled view must apply to undo the substitution.
    SwizzleFix swizzle_fix{SwizzleFix::None};
    /// True when the format can be a render target on this device.
    bool attachable{};
    /// True when the format can be bound for shader writes.
    bool storage{};
};

/// Whether this device can sample @p pixel_format's compressed family directly.
/// False means SurfaceFormat substituted an uncompressed format and the shared
/// cache has to decode into it, see ImageFlagBits::Converted.
[[nodiscard]] bool IsCompressedFamilyNative(const Device& device,
                                           VideoCore::Surface::PixelFormat pixel_format);

/// Resolves a guest pixel format to a Metal one. Takes the device because the
/// answer varies across the hardware we target, D24S8 does not exist on Apple
/// GPUs and ASTC and ETC2 are absent on Intel Macs. Callers have to honour
/// FormatSupport instead of assuming the returned format is a drop-in.
[[nodiscard]] FormatInfo SurfaceFormat(const Device& device,
                                       VideoCore::Surface::PixelFormat pixel_format);

/// Vertex attribute format for a Maxwell size/type pair.
/// Returns MTL::VertexFormatInvalid for combinations Metal cannot express.
[[nodiscard]] MTL::VertexFormat VertexFormat(
    Tegra::Engines::Maxwell3D::Regs::VertexAttribute::Size size,
    Tegra::Engines::Maxwell3D::Regs::VertexAttribute::Type type);

/// Size in bytes of a single element of @p format, or 0 if it is not one of the
/// formats VertexFormat() can produce. Metal validates an attribute against its
/// layout's stride, so the declared stride must cover offset + element size.
[[nodiscard]] u32 VertexFormatSize(MTL::VertexFormat format);

[[nodiscard]] MTL::CompareFunction ComparisonOp(
    Tegra::Engines::Maxwell3D::Regs::ComparisonOp comparison);

/// Sampler depth-compare uses its own, smaller enum than the depth test does.
[[nodiscard]] MTL::CompareFunction DepthCompareFunction(
    Tegra::Texture::DepthCompareFunc func);

[[nodiscard]] MTL::SamplerAddressMode WrapMode(const Device& device,
                                               Tegra::Texture::WrapMode wrap_mode,
                                               Tegra::Texture::TextureFilter filter);

[[nodiscard]] MTL::SamplerMinMagFilter SamplerFilter(Tegra::Texture::TextureFilter filter);

[[nodiscard]] MTL::SamplerMipFilter SamplerMipFilter(Tegra::Texture::TextureMipmapFilter filter);

[[nodiscard]] MTL::SamplerReductionMode SamplerReductionMode(
    Tegra::Texture::SamplerReduction reduction);

[[nodiscard]] MTL::PrimitiveType PrimitiveTopology(
    const Device& device, Tegra::Engines::Maxwell3D::Regs::PrimitiveTopology topology);

[[nodiscard]] MTL::BlendFactor BlendFactor(Tegra::Engines::Maxwell3D::Regs::Blend::Factor factor);

[[nodiscard]] MTL::BlendOperation BlendEquation(
    Tegra::Engines::Maxwell3D::Regs::Blend::Equation equation);

[[nodiscard]] MTL::CullMode CullFace(Tegra::Engines::Maxwell3D::Regs::CullFace cull_face);

[[nodiscard]] MTL::StencilOperation StencilOp(
    Tegra::Engines::Maxwell3D::Regs::StencilOp::Op stencil_op);

[[nodiscard]] MTL::Winding FrontFace(Tegra::Engines::Maxwell3D::Regs::FrontFace front_face);

} // namespace Metal
