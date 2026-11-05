// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include "yuzu_video_core/presentation_status.h"

namespace Settings {
bool IsLegacyFsrEnabled();
}

namespace VideoCore {

struct FsrFrameState {
    bool legacy{};
    unsigned segment{};
    unsigned input_width{}, input_height{}, offscreen_passes{};
    int sharpness_stored{25}, cas_strength{};
    unsigned selected_filter{5};
    unsigned viewport_width{}, viewport_height{};
    int downscale_filter{};
    unsigned effective_filter{}, scale_mode{};
    bool selection_noted{};
};
inline thread_local FsrFrameState fsr_frame_state;

// Snapshot on the renderer thread: a UI change takes effect between complete
// frames, never halfway through configuring the shaders of a layer.
inline void BeginFsrFrame(int sharpness = 25, int cas_strength = 0, unsigned selected_filter = 5, int downscale_filter = 0) {
    const bool legacy = Settings::IsLegacyFsrEnabled();
    if (legacy != fsr_frame_state.legacy || sharpness != fsr_frame_state.sharpness_stored ||
        cas_strength != fsr_frame_state.cas_strength || selected_filter != fsr_frame_state.selected_filter || downscale_filter != fsr_frame_state.downscale_filter)
        ++fsr_frame_state.segment;
    fsr_frame_state.legacy = legacy;
    fsr_frame_state.input_width = fsr_frame_state.input_height = fsr_frame_state.offscreen_passes = 0;
    fsr_frame_state.viewport_width = fsr_frame_state.viewport_height = 0;
    fsr_frame_state.sharpness_stored = sharpness;
    fsr_frame_state.cas_strength = cas_strength;
    fsr_frame_state.selected_filter = selected_filter;
    fsr_frame_state.downscale_filter = downscale_filter;
    fsr_frame_state.selection_noted = false;
}
inline void NotePresentSelection(unsigned filter, unsigned mode) {
    // A frame can contain multiple layers. Dimensions and effective mode in the
    // CSV describe its first layer; total GPU time still covers all layers.
    if (fsr_frame_state.selection_noted) return;
    fsr_frame_state.selection_noted = true;
    fsr_frame_state.effective_filter = filter;
    fsr_frame_state.scale_mode = mode;
}
inline void PublishPresentationStatus(bool applet, unsigned output_width, unsigned output_height, unsigned layers) {
    if (applet) return;
    VideoPresentationState state{};
    if (fsr_frame_state.selection_noted && fsr_frame_state.viewport_width && fsr_frame_state.viewport_height &&
        output_width && output_height && layers) {
        switch (fsr_frame_state.scale_mode) {
        case 0: state.mode = VideoPresentationMode::Native; break;
        case 1: state.mode = VideoPresentationMode::Upscaling; break;
        case 2: state.mode = VideoPresentationMode::Downscaling; break;
        default: break;
        }
        if (state.mode != VideoPresentationMode::Unknown) {
            state.input_width = fsr_frame_state.viewport_width;
            state.input_height = fsr_frame_state.viewport_height;
            state.output_width = output_width;
            state.output_height = output_height;
            state.layers = layers;
            state.filter = static_cast<ScalingFilter>(fsr_frame_state.effective_filter);
        }
    }
    presentation_status.Publish(state);
}
inline bool LegacyFsr() {
    return fsr_frame_state.legacy;
}
inline void NoteFsrInput(unsigned width, unsigned height, unsigned offscreen_passes) {
    if (!fsr_frame_state.viewport_width) {
        fsr_frame_state.input_width = width;
        fsr_frame_state.input_height = height;
    }
    fsr_frame_state.offscreen_passes += offscreen_passes;
}
inline void NotePresentInput(unsigned width, unsigned height, unsigned viewport_width, unsigned viewport_height) {
    if (fsr_frame_state.selection_noted) return;
    fsr_frame_state.input_width = width;
    fsr_frame_state.input_height = height;
    fsr_frame_state.viewport_width = viewport_width;
    fsr_frame_state.viewport_height = viewport_height;
}

// Opt-in diagnostics. No query allocation, GPU wait, or file access in normal use.
class PresentDiagnostics {
  public:
    using Clock = std::chrono::steady_clock;
    struct Sample {
        unsigned long long frame{};
        unsigned width{}, height{}, layers{}, filter{};
        double cpu_ms{};
        bool legacy{};
        unsigned segment{}, input_width{}, input_height{}, offscreen_passes{};
        double elapsed_s{}, frame_interval_ms{};
        int sharpness_stored{}, cas_strength{};
        unsigned selected_filter{}, viewport_width{}, viewport_height{};
        bool is_applet{};
        int downscale_filter{};
        unsigned scale_mode{};
    };

    static bool Enabled() {
        static const bool enabled = [] {
            const char *path = std::getenv("NXEMU_FSR_METRICS");
            return path && *path;
        }();
        return enabled;
    }

    static Sample Begin(unsigned width, unsigned height, unsigned layers, unsigned filter) {
        static std::atomic<unsigned long long> next{};
        static const auto origin = Clock::now();
        return {next.fetch_add(1), width, height, layers, fsr_frame_state.selection_noted ? fsr_frame_state.effective_filter : filter, 0.0,
                LegacyFsr(), fsr_frame_state.segment, fsr_frame_state.input_width,
                fsr_frame_state.input_height, fsr_frame_state.offscreen_passes,
                std::chrono::duration<double>(Clock::now() - origin).count(), 0.0,
                fsr_frame_state.sharpness_stored, fsr_frame_state.cas_strength,
                fsr_frame_state.selected_filter, fsr_frame_state.viewport_width, fsr_frame_state.viewport_height, false, fsr_frame_state.downscale_filter, fsr_frame_state.scale_mode};
    }

    static void Write(const char *backend, const Sample &sample, double gpu_ms) {
        static std::mutex mutex;
        const std::scoped_lock lock(mutex);
        // Each run should use a fresh file. Several display/capture instances share it.
        static FILE *file = [] {
            FILE *output = std::fopen(std::getenv("NXEMU_FSR_METRICS"), "w");
            if (output)
                std::fputs(
                    "frame,backend,implementation,filter,width,height,layers,cpu_ms,gpu_ms,segment,input_width,input_height,offscreen_passes,elapsed_s,frame_interval_ms,sharpness_stored,cas_strength,selected_filter,viewport_width,viewport_height,target,downscale_filter,scale_mode\n",
                    output);
            return output;
        }();
        if (file) {
            const bool is_fsr = sample.filter == 5 || sample.filter == 6;
            std::fprintf(file, "%llu,%s,%s,%u,%u,%u,%u,%.6f,%.6f,%u,%u,%u,%u,%.6f,%.6f,%d,%d,%u,%u,%u,%s,%d,%s\n", sample.frame, backend,
                         is_fsr ? (sample.legacy ? "legacy" : "fused") : "standard", sample.filter, sample.width,
                         sample.height, sample.layers, sample.cpu_ms, gpu_ms, sample.segment,
                         sample.input_width, sample.input_height, sample.offscreen_passes,
                         sample.elapsed_s, sample.frame_interval_ms, sample.sharpness_stored, sample.cas_strength,
                         sample.selected_filter, sample.viewport_width, sample.viewport_height, sample.is_applet ? "applet" : "display", sample.downscale_filter,
                         sample.scale_mode == 2 ? "downscale" : sample.scale_mode == 1 ? "upscale" : "native");
            std::fflush(file);
        }
    }
};

} // namespace VideoCore
