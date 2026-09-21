// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/am/applet.h"
#include "core/hle/service/am/service/applet_common_functions.h"
#include "core/hle/service/cmif_serialization.h"

namespace Service::AM {

IAppletCommonFunctions::IAppletCommonFunctions(Core::System& system_,
                                               std::shared_ptr<Applet> applet_)
    : ServiceFramework{system_, "IAppletCommonFunctions"}, applet{std::move(applet_)} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetTerminateResult"},
        {10, nullptr, "ReadThemeStorage"},
        {11, nullptr, "WriteThemeStorage"},
        {20, nullptr, "PushToAppletBoundChannel"},
        {21, nullptr, "TryPopFromAppletBoundChannel"},
        {40, nullptr, "GetDisplayLogicalResolution"},
        {42, D<&IAppletCommonFunctions::SetDisplayMagnification>, "SetDisplayMagnification"},
        {50, D<&IAppletCommonFunctions::SetHomeButtonDoubleClickEnabled>, "SetHomeButtonDoubleClickEnabled"},
        {51, D<&IAppletCommonFunctions::GetHomeButtonDoubleClickEnabled>, "GetHomeButtonDoubleClickEnabled"},
        {52, nullptr, "IsHomeButtonShortPressedBlocked"},
        {60, nullptr, "IsVrModeCurtainRequired"},
        {61, nullptr, "IsSleepRequiredByHighTemperature"},
        {62, nullptr, "IsSleepRequiredByLowBattery"},
        {70, D<&IAppletCommonFunctions::SetCpuBoostRequestPriority>, "SetCpuBoostRequestPriority"},
        {80, nullptr, "SetHandlingCaptureButtonShortPressedMessageEnabledForApplet"},
        {81, nullptr, "SetHandlingCaptureButtonLongPressedMessageEnabledForApplet"},
        {90, nullptr, "OpenNamedChannelAsParent"},
        {91, nullptr, "OpenNamedChannelAsChild"},
        {100, nullptr, "SetApplicationCoreUsageMode"},
        {300, D<&IAppletCommonFunctions::GetCurrentApplicationId>, "GetCurrentApplicationId"},
        {320, D<&IAppletCommonFunctions::SetGpuTimeSliceBoost>, "SetGpuTimeSliceBoost"},
        {350, D<&IAppletCommonFunctions::Unknown350>, "Unknown350"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

IAppletCommonFunctions::~IAppletCommonFunctions() = default;

Result IAppletCommonFunctions::SetHomeButtonDoubleClickEnabled(
    bool home_button_double_click_enabled) {
    LOG_WARNING(Service_AM, "(STUBBED) called, home_button_double_click_enabled={}",
                home_button_double_click_enabled);
    R_SUCCEED();
}

Result IAppletCommonFunctions::SetDisplayMagnification(f32 x, f32 y, f32 width, f32 height) {
    LOG_DEBUG(Service_AM, "(STUBBED) called, x={}, y={}, width={}, height={}", x, y, width,
              height);
    std::scoped_lock lk{applet->lock};
    applet->display_magnification = Common::Rectangle<f32>{x, y, x + width, y + height};
    R_SUCCEED();
}

Result IAppletCommonFunctions::GetHomeButtonDoubleClickEnabled(
    Out<bool> out_home_button_double_click_enabled) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_home_button_double_click_enabled = false;
    R_SUCCEED();
}

Result IAppletCommonFunctions::SetCpuBoostRequestPriority(s32 priority) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    std::scoped_lock lk{applet->lock};
    applet->cpu_boost_request_priority = priority;
    R_SUCCEED();
}

Result IAppletCommonFunctions::GetCurrentApplicationId(Out<u64> out_application_id) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_application_id = system.GetApplicationProcessProgramID() & ~0xFFFULL;
    R_SUCCEED();
}

Result IAppletCommonFunctions::SetGpuTimeSliceBoost(s64 time_span) {
    LOG_WARNING(Service_AM, "(STUBBED) called, time_span={}", time_span);
    R_SUCCEED();
}

Result IAppletCommonFunctions::Unknown350(Out<u16> out_unknown) {
    LOG_WARNING(Service_AM, "(STUBBED) called");
    *out_unknown = 0;
    R_SUCCEED();
}

} // namespace Service::AM
