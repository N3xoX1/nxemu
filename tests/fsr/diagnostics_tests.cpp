// SPDX-License-Identifier: GPL-2.0-or-later
#include "nxemu-module-spec/fsr_sharpness.h"
#include "yuzu_video_core/fsr_diagnostics.h"
#include <stdexcept>
#include <cmath>

namespace {
bool requested_legacy = false;
void Require(bool condition) {
    if (!condition) throw std::runtime_error("FSR diagnostics regression");
}
}
namespace Settings {
bool IsLegacyFsrEnabled() { return requested_legacy; }
}

int main() {
    VideoCore::BeginFsrFrame();
    Require(!VideoCore::LegacyFsr());
    requested_legacy = true;
    Require(!VideoCore::LegacyFsr()); // UI updates cannot change a frame in progress.
    VideoCore::BeginFsrFrame();
    Require(VideoCore::LegacyFsr());
    VideoCore::NoteFsrInput(1280, 720, 2);
    const auto old = VideoCore::PresentDiagnostics::Begin(3840, 2160, 1, 5);
    requested_legacy = false;
    VideoCore::BeginFsrFrame();
    Require(!VideoCore::LegacyFsr());
    VideoCore::NoteFsrInput(1280, 720, 1);
    const auto current = VideoCore::PresentDiagnostics::Begin(3840, 2160, 1, 5);
    Require(old.legacy && !current.legacy); // Delayed GPU results keep the original label.
    Require(old.segment + 1 == current.segment);
    Require(old.offscreen_passes == 2 && current.offscreen_passes == 1);
    Require(old.input_width == 1280 && old.input_height == 720);
    const auto segment = current.segment;
    VideoCore::BeginFsrFrame();
    const auto unchanged = VideoCore::PresentDiagnostics::Begin(3840, 2160, 1, 5);
    Require(unchanged.segment == segment && unchanged.offscreen_passes == 0);
    VideoCore::BeginFsrFrame(68, 30, 7);
    VideoCore::NotePresentInput(1600, 900, 1280, 720);
    const auto catmull = VideoCore::PresentDiagnostics::Begin(1920, 1080, 1, 11);
    Require(catmull.segment == segment + 1 && catmull.cas_strength == 30);
    Require(catmull.selected_filter == 7 && catmull.sharpness_stored == 68);
    Require(catmull.viewport_width == 1280 && catmull.viewport_height == 720);
    VideoCore::BeginFsrFrame(68, 30, 7, 1);
    VideoCore::NotePresentInput(3840,2160,3840,2160);
    VideoCore::NotePresentSelection(20,2);
    VideoCore::NotePresentSelection(1,0);
    const auto area = VideoCore::PresentDiagnostics::Begin(1920,1080,2,7);
    Require(area.filter==20 && area.selected_filter==7 && area.downscale_filter==1 && area.scale_mode==2);
    Require(area.segment==catmull.segment+1);
    for (int i = 0; i <= 100; ++i)
        Require(FsrSharpness::ToDisplay(FsrSharpness::ToStored(i)) == i);
    Require(FsrSharpness::ToDisplay(-1) == 100 && FsrSharpness::ToDisplay(201) == 0);
    Require(FsrSharpness::ToDisplay(25) == 88 && FsrSharpness::ToStored(0) == 200);
    for (int legacy = 0; legacy <= 200; ++legacy) {
        const auto strength = SharpeningStrength::FromLegacyRcas(legacy);
        Require(std::abs(strength.RcasAttenuationStops() - legacy / 100.0f) < 0.000001f);
    }
    Require(SharpeningStrength::FromPercent(-1).IsOff());
    Require(SharpeningStrength::FromPercent(101).CasUniform() == 1.0f);
    Require(SharpeningStrength::FromLegacyRcas(25).Percent() == 87.5f);
    VideoCore::PublishPresentationStatus(false, 1920, 1080, 2);
    auto status = VideoCore::presentation_status.Read();
    Require(status.mode == VideoPresentationMode::Downscaling && status.input_width == 3840 && status.layers == 2);
    Require(status.filter == ScalingFilter::Area);
    VideoCore::BeginFsrFrame();
    VideoCore::PublishPresentationStatus(true, 320, 240, 1);
    Require(VideoCore::presentation_status.Read().input_width == 3840);
    VideoCore::NotePresentInput(1280, 720, 1280, 720);
    VideoCore::NotePresentSelection(5, 1);
    VideoCore::PublishPresentationStatus(false, 1920, 1080, 1);
    status = VideoCore::presentation_status.Read();
    Require(status.mode == VideoPresentationMode::Upscaling && status.filter == ScalingFilter::Fsr);
    VideoCore::PublishPresentationStatus(false, 0, 0, 0);
    Require(VideoCore::presentation_status.Read().mode == VideoPresentationMode::Unknown);
    VideoCore::presentation_status.Reset();
    Require(VideoCore::presentation_status.Read().input_width == 0);
}
