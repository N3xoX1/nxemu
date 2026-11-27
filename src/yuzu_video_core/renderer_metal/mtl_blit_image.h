// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <unordered_map>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/engines/fermi_2d.h"

#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/renderer_metal/mtl_render_pass_state.h"
#include "yuzu_video_core/texture_cache/types.h"

namespace Metal {

class Device;
class Scheduler;

// Texture-to-texture blits, done as a draw. A blit encoder can only copy
// matching regions of matching formats, and the guest routinely asks for a
// scaled, filtered, format-converting or Y-flipped copy, all of which one
// textured draw handles. Each kind of draw keeps its own pipeline cache, keyed
// on whatever that pipeline state bakes in.
class BlitImageHelper : Common::NonCopyable, Common::NonMoveable {
public:
    explicit BlitImageHelper(const Device& device, Scheduler& scheduler);
    ~BlitImageHelper();

    /// Which direction a conversion draw runs in.
    enum class ConvertKind {
        DepthToColor,
        ColorToDepth,
        DepthToDepth,
    };

    /// Reinterprets one aspect as the other, as a draw, since a blit encoder
    /// cannot cross the depth/colour boundary or rescale. Stencil is not
    /// covered, Metal has no stencil export, so a packed destination keeps the
    /// stencil it already had.
    void Convert(const RenderPassState& pass, MTL::Texture* src, ConvertKind kind,
                 const VideoCommon::Region2D& dst_region,
                 const VideoCommon::Region2D& src_region, bool linear);

    /// Writes every sample of a multisampled image from a single-sample one.
    /// Supports both 1:1 broadcast expand and Maxwell's spatial sample-grid pack
    /// (e.g. 3840x2160 1x -> 1920x1080 4x via [[sample_id]]).
    void ExpandToMultisample(MTL::Texture* dst, MTL::Texture* src);

    /// Unpacks every sample of a multisampled image into a larger single-sample
    /// image laid out in Maxwell's spatial sample grid (e.g. 1920x1080 4x -> 3840x2160 1x).
    void UnpackFromMultisample(MTL::Texture* dst, MTL::Texture* src);

    /// One rectangle to expand, in texels. Source and destination extents match,
    /// an expand changes the sample count and never the size.
    struct ExpandRegion {
        u32 src_x;
        u32 src_y;
        u32 dst_x;
        u32 dst_y;
        u32 width;
        u32 height;
    };

    /// The same expand, restricted to @p regions. The attachment is loaded
    /// instead of discarded and each region is drawn under its own viewport and
    /// scissor, so nothing outside it is touched. Separate from the whole-image
    /// form on purpose, that one discards the attachment, which is cheaper and
    /// what an upload means.
    void ExpandToMultisample(MTL::Texture* dst, MTL::Texture* src,
                             std::span<const ExpandRegion> regions);

    /// Guest bit layout produced by PackDepthStencil.
    enum class DepthPackLayout : u32 {
        S8D24 = 0, ///< Maxwell Z24_UNORM_S8_UINT: depth in bits 8..31, stencil in 0..7
        D24S8 = 1, ///< Maxwell S8Z24_UNORM: depth in bits 0..23, stencil in 24..31
        X8D24 = 2, ///< Maxwell X8Z24_UNORM: depth in bits 0..23, upper byte 0
        D32S8 = 3, ///< Maxwell Z32_FLOAT_X24S8_UINT: 64-bit [float32, u8 stencil + 3B pad]
    };

    /// Packs a separate depth plane and stencil plane into the guest's depth/stencil layout.
    /// Metal copies one aspect of a depth/stencil texture at a time, so a readback arrives as
    /// separate planes. Runs as a kernel because the planes do not exist until the blits
    /// producing them have executed.
    /// @p dst_row_texels and @p plane_row_texels are texel counts and not bytes.
    void PackDepthStencil(MTL::Buffer* dst, u64 dst_offset, u32 dst_row_texels,
                          MTL::Buffer* planes, u64 depth_offset, u64 stencil_offset,
                          u32 plane_row_texels, u32 width, u32 height,
                          DepthPackLayout layout = DepthPackLayout::S8D24);

    /// Clears part of the already-open pass's attachments, as a draw. Metal has
    /// no clear-rect, so a clear restricted to the scissor rectangle has to be
    /// drawn. The rectangle is in attachment texels.
    /// @p raw_color holds the four raw 32-bit register words (f32, u32, or s32
    /// depending on the attachment pixel format).
    /// @p color_mask is an MTL::ColorWriteMask and @p stencil_mask the guest's
    /// stencil write mask, so a partial clear leaves unnamed channels and bit
    /// planes untouched.
    void ClearAttachments(const RenderPassState& pass, u32 color_target, bool clear_color,
                          bool clear_depth, bool clear_stencil,
                          const std::array<u32, 4>& raw_color, f32 depth, u32 stencil,
                          s32 left, s32 top, s32 right, s32 bottom,
                          u32 color_mask = 0xf, u32 stencil_mask = 0xff,
                          MTL::Buffer* indirect_buffer = nullptr, u32 indirect_offset = 0);

    /// Records a blit from @p src into @p dst. Regions are in texel coordinates
    /// of their own images. A source or destination region whose start exceeds its
    /// end on an axis flips that axis. When @p scissor_clip is set, only the
    /// scissor is clipped (the viewport stays full-size so UVs are preserved).
    /// When @p override_sampler is non-null, it is used instead of the internal
    /// nearest/bilinear clamp-to-edge sampler.
    void BlitColor(MTL::Texture* dst, MTL::Texture* src, const VideoCommon::Region2D& dst_region,
                   const VideoCommon::Region2D& src_region,
                   Tegra::Engines::Fermi2D::Filter filter,
                   const std::optional<MTL::ScissorRect>& scissor_clip = std::nullopt,
                   MTL::SamplerState* override_sampler = nullptr);

    [[nodiscard]] bool IsValid() const noexcept {
        return static_cast<bool>(library);
    }

private:
    /// Returns (building if needed) the pipeline writing to @p format.
    [[nodiscard]] MTL::RenderPipelineState* GetPipeline(MTL::PixelFormat format);

    /// Pipeline for an expand draw into a @p samples-sample @p format target.
    [[nodiscard]] MTL::RenderPipelineState* GetExpandPipeline(MTL::PixelFormat format,
                                                              u32 samples);

    /// Pipeline for an unpack draw from an MSAA texture into a 1x @p format target.
    [[nodiscard]] MTL::RenderPipelineState* GetUnpackPipeline(MTL::PixelFormat format);

    /// Pipeline for a drawn clear, matching the pass's attachment formats and
    /// writing only @p color_target, masked by @p color_mask.
    [[nodiscard]] MTL::RenderPipelineState* GetClearPipeline(const RenderPassState& pass,
                                                             u32 color_target, bool clear_color,
                                                             u32 color_mask);

    [[nodiscard]] MTL::RenderPipelineState* GetConvertPipeline(const RenderPassState& pass,
                                                               ConvertKind kind);

    /// Built on first use, a title that never reads back a depth/stencil
    /// surface never pays for compiling it.
    [[nodiscard]] MTL::ComputePipelineState* GetPackDepthStencilPipeline();

    Owned<MTL::ComputePipelineState> pack_depth_stencil_pipeline;

    std::unordered_map<u32, Owned<MTL::RenderPipelineState>> expand_pipelines;
    std::unordered_map<u32, Owned<MTL::RenderPipelineState>> unpack_pipelines;
    std::unordered_map<u64, Owned<MTL::RenderPipelineState>> convert_pipelines;
    std::unordered_map<u32, Owned<MTL::DepthStencilState>> convert_depth_states;
    std::unordered_map<u64, Owned<MTL::RenderPipelineState>> clear_pipelines;
    std::unordered_map<u32, Owned<MTL::DepthStencilState>> clear_depth_states;

    const Device& device;
    Scheduler& scheduler;

    Owned<MTL::Library> library;
    Owned<MTL::SamplerState> sampler_linear;
    Owned<MTL::SamplerState> sampler_nearest;

    std::unordered_map<u32, Owned<MTL::RenderPipelineState>> pipelines;
};

} // namespace Metal
