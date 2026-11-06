// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nxemu-module-spec/presentation_profile.h"
#include "nxemu-video/video_settings_identifiers.h"

inline constexpr bool IsManualScalingFilter(ScalingFilter filter) {
    return static_cast<unsigned>(filter) <= static_cast<unsigned>(ScalingFilter::ScaleForceFast);
}
inline constexpr auto RetiredMetalFxFilter = static_cast<ScalingFilter>(12);

inline constexpr ScalingFilter ValidCustomScalingFilter(ScalingFilter filter) {
    if (filter == RetiredMetalFxFilter) return ScalingFilter::Fsr;
    return IsManualScalingFilter(filter) ? filter : ScalingFilter::Bilinear;
}
template <typename Settings>
ScalingFilter CustomScalingFilter(const Settings& settings) {
    const auto active = static_cast<ScalingFilter>(settings.GetInt(NXVideoSetting::SystemScalingFilter));
    return ValidCustomScalingFilter(IsAutomaticScalingFilter(active)
        ? static_cast<ScalingFilter>(settings.GetInt(NXVideoSetting::CustomScalingFilter)) : active);
}
// Store and SettingsContext expose the same key-based API, including per-game contexts.
template <typename Settings>
void SelectPresentationFilter(Settings& settings, ScalingFilter next) {
    if (!IsAutomaticScalingFilter(next) && !IsManualScalingFilter(next)) return;
    const auto active = static_cast<ScalingFilter>(settings.GetInt(NXVideoSetting::SystemScalingFilter));
    if (IsManualScalingFilter(next))
        settings.SetInt(NXVideoSetting::CustomScalingFilter, static_cast<int>(next));
    else if (IsManualScalingFilter(active))
        settings.SetInt(NXVideoSetting::CustomScalingFilter, static_cast<int>(active));
    settings.SetInt(NXVideoSetting::SystemScalingFilter, static_cast<int>(next));
}
template <typename Settings>
void ActivateCustomPresentation(Settings& settings) {
    SelectPresentationFilter(settings, CustomScalingFilter(settings));
}
