// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <unordered_map>

#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/math_util.h"
#include "yuzu_video_core/framebuffer_config.h"
#include "yuzu_video_core/metal_common/metal_wrapper.h"
#include "yuzu_video_core/renderer_metal/mtl_render_pass_state.h"

namespace Metal {

class Device;
class Scheduler;

/// Draws the guest's composited framebuffer into the swapchain drawable. It's a
/// full-screen triangle instead of a blit encoder copy because the source and
/// destination differ in size, format and orientation. The shaders are inline
/// MSL, they're fixed and carry no guest state so they skip the recompiler.
class BlitScreen : Common::NonCopyable, Common::NonMoveable {
public:
    explicit BlitScreen(const Device& device, Scheduler& scheduler);
    ~BlitScreen();

    /// Whether the pipeline compiled. If false the caller should clear the
    /// drawable instead of presenting undefined contents.
    [[nodiscard]] bool IsValid() const noexcept {
        // This means the shader library compiled, not that a pipeline exists.
        // Those are built lazily on the first Draw once we know the destination
        // format.
        return static_cast<bool>(library);
    }

    /// Records the full-screen draw. `source` is the guest framebuffer, `target`
    /// the drawable we draw into. crop_left..crop_bottom are a normalised source
    /// rect, so a guest framebuffer larger than the visible area gets cropped
    /// and not squashed. The base layer is the bottom of the composition stack,
    /// it owns the drawable and is the only one the scaling/AA chain runs for,
    /// overlays load and blend over it with `blending`. `screen` is where in the
    /// drawable the frame goes, outside it we clear to the background colour so
    /// a window with a different aspect ratio letterboxes instead of stretching.
    void Draw(MTL::Texture* source, MTL::Texture* target, u32 target_width, u32 target_height,
              f32 crop_left, f32 crop_top, f32 crop_right, f32 crop_bottom,
              Tegra::BlendMode blending = Tegra::BlendMode::Opaque,
              bool is_base_layer = true, const Common::Rectangle<u32>& screen = {},
              bool apply_post_processing = true);

    /// Which fragment program the present pass runs. Nearest and bilinear aren't
    /// in here, those are just sampler state over the plain program. The rest
    /// rebuild the source from several texels so each needs its own function.
    enum class Filter : u32 {
        Standard = 0,
        Cubic,
        Gaussian,
        Lanczos,
        Area,
        ScaleForce,
        Spline1,
        Mmpx,
        Sgsr,
        SgsrEdge,
        /// FSR is two passes, EASU reconstructs at output resolution and RCAS
        /// sharpens what EASU made. Neither one is selectable on its own.
        FsrEasu,
        FsrRcas,
        /// SMAA is three passes, edge detection, blending weights (needs the two
        /// precomputed lookup textures), then neighbourhood blending.
        SmaaEdges,
        SmaaWeights,
        SmaaBlend,
        Fxaa,
    };

private:
    /// Runs SMAA's three passes and returns the antialiased image, or @p source
    /// untouched if the LUTs or targets can't be allocated. Runs at source
    /// resolution before any scaling filter, SMAA rebuilds edges from the
    /// geometry that made them.
    [[nodiscard]] MTL::Texture* SmaaAntiAlias(MTL::Texture* source);

    /// Uploads the SMAA area and search lookup textures. Both are compile-time
    /// constant data so this only runs once.
    void EnsureSmaaLuts();

    /// Upscales @p source to the target size with MetalFX, or returns @p source
    /// untouched when MetalFX can't or shouldn't be used. The scaler only takes
    /// an input content size, there's no input origin, so we can only express
    /// the crop's extent. A crop that doesn't start at the top-left falls back
    /// to the normal filter path instead of scaling the wrong region.
    [[nodiscard]] MTL::Texture* MetalFxUpscale(MTL::Texture* source, u32 target_width,
                                               u32 target_height, f32 crop_left, f32 crop_top,
                                               f32 crop_right, f32 crop_bottom);

    /// Runs FSR's EASU pass into an intermediate target and returns it, so the
    /// present pass after it only has to run RCAS over the result. Returns
    /// @p source untouched if the target can't be allocated, that way the
    /// caller's path is the same either way.
    [[nodiscard]] MTL::Texture* FsrUpscale(MTL::Texture* source, u32 target_width,
                                           u32 target_height, f32 crop_left, f32 crop_top,
                                           f32 crop_right, f32 crop_bottom);

    /// Runs the FXAA pass and returns what the present pass should sample. That
    /// is the source itself when FXAA is off or its target can't be allocated,
    /// so the caller needs no branch. SMAA has its own entry point.
    [[nodiscard]] MTL::Texture* AntiAlias(MTL::Texture* source);

    /// Builds (or returns) the pipeline for a target format, filter and blend
    /// mode. Null if it couldn't be created, and the caller falls back rather
    /// than skipping the frame.
    [[nodiscard]] MTL::RenderPipelineState* GetPipeline(
        MTL::PixelFormat target_format, Filter filter,
        Tegra::BlendMode blending = Tegra::BlendMode::Opaque);

    const Device& device;
    Scheduler& scheduler;

    Owned<MTL::Library> library;
    /// Keyed by target format, blend mode and filter. The user can change the
    /// filter at any time and changing it back shouldn't recompile.
    std::unordered_map<u64, Owned<MTL::RenderPipelineState>> pipelines;
    Owned<MTL::SamplerState> sampler_linear;
    Owned<MTL::SamplerState> sampler_nearest;

    /// FXAA target, rebuilt when the guest framebuffer changes size or format.
    Owned<MTL::Texture> aa_texture;

    /// EASU's output, held at display resolution between the two FSR passes.
    Owned<MTL::Texture> fsr_texture;
    u32 fsr_width{};
    u32 fsr_height{};

    /// MetalFX spatial upscale, built through the Metal 4 compiler.
    Owned<MTL4FX::SpatialScaler> metalfx_scaler_mtl4;
    Owned<MTL::Texture> metalfx_output;
    u32 metalfx_input_width{};
    u32 metalfx_input_height{};
    u32 metalfx_output_width{};
    u32 metalfx_output_height{};
    MTL::PixelFormat metalfx_input_format{MTL::PixelFormatInvalid};
    /// Input content region the scaler was built for. Changing it needs a
    /// rebuild, the scaler ignores a later one.
    u32 metalfx_content_width{};
    u32 metalfx_content_height{};

    /// SMAA's two intermediates and its two constant lookup textures.
    Owned<MTL::Texture> smaa_edges;
    Owned<MTL::Texture> smaa_weights;
    Owned<MTL::Texture> smaa_output;
    Owned<MTL::Texture> smaa_area_lut;
    Owned<MTL::Texture> smaa_search_lut;
    u32 smaa_width{};
    u32 smaa_height{};
    u32 aa_width{};
    u32 aa_height{};
    MTL::PixelFormat aa_format{MTL::PixelFormatInvalid};
};

} // namespace Metal
