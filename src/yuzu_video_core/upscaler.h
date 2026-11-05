// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cmath>
#include <cstdint>
#include <optional>

namespace VideoCore::Upscaling {
enum class Backend { Fsr1, MetalFxSpatial };
enum class ColorEncoding { DisplayReferred, Linear, Hdr };
enum class MotionVectorUnits { InputPixels, OutputPixels, Normalized };
enum class MotionVectorDirection { CurrentToPrevious, PreviousToCurrent };
struct Extent { std::uint32_t width{}, height{}; };

// Describes the resource contract, not a guessed guest shader or game ID.
// Actual texture handles belong to the renderer adapter and retain their own lifetime.
struct TemporalParameters {
    bool has_depth{}, has_motion_vectors{}, has_jitter{};
    float jitter_x{}, jitter_y{}; // Input-pixel offsets, before upscaling.
    float motion_scale_x{1}, motion_scale_y{1};
    MotionVectorUnits motion_units{MotionVectorUnits::InputPixels};
    MotionVectorDirection motion_direction{MotionVectorDirection::CurrentToPrevious};
    bool motion_y_down{true};
    bool depth_reversed{}, reset_history{};
    std::uint64_t frame_id{};
};
struct FrameParameters {
    Extent input{}, output{};
    ColorEncoding encoding{ColorEncoding::DisplayReferred};
    bool opaque{true};
    std::optional<TemporalParameters> temporal;
};
struct Capabilities {
    bool temporal{}, hdr{}, alpha{};
    bool native_resolution{};
    bool cpu_synchronized{}; // Prototype interop: not a production-performance path.
};
enum class Rejection { None, EmptyExtent, NotUpscaling, ColorEncoding, Alpha, TemporalInputs };
inline Rejection Validate(const Capabilities& caps, const FrameParameters& frame) {
    if (!frame.input.width || !frame.input.height || !frame.output.width || !frame.output.height)
        return Rejection::EmptyExtent;
    if (frame.output.width < frame.input.width || frame.output.height < frame.input.height ||
        (!caps.native_resolution && frame.input.width == frame.output.width && frame.input.height == frame.output.height))
        return Rejection::NotUpscaling;
    if (frame.encoding == ColorEncoding::Hdr && !caps.hdr) return Rejection::ColorEncoding;
    if (!frame.opaque && !caps.alpha) return Rejection::Alpha;
    if (caps.temporal) {
        if (!frame.temporal || !frame.temporal->has_depth || !frame.temporal->has_motion_vectors || !frame.temporal->has_jitter)
            return Rejection::TemporalInputs;
        const auto& t = *frame.temporal;
        if (!std::isfinite(t.jitter_x) || !std::isfinite(t.jitter_y) ||
            !std::isfinite(t.motion_scale_x) || !std::isfinite(t.motion_scale_y) ||
            t.motion_scale_x == 0 || t.motion_scale_y == 0)
            return Rejection::TemporalInputs;
    }
    return Rejection::None;
}
} // namespace VideoCore::Upscaling
