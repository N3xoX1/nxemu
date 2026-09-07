// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_input_common/drivers/android.h"
#include "yuzu_common/android/java_bridge.h"
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include <yuzu_common/thread.h>

#ifdef ANDROID
#include <jni.h>
#endif

namespace InputCommon
{

Android::Android(std::string input_engine_) :
    InputEngine(std::move(input_engine_))
{
    vibration_thread = std::jthread([this](std::stop_token token) {
        Common::SetCurrentThreadName("Android_Vibration");
#ifdef ANDROID
        auto env = GetEnvForThread();
        using namespace std::chrono_literals;
        while (!token.stop_requested())
        {
            SendVibrations(env, token);
        }
#endif
    });
}

Android::~Android() = default;

#ifdef ANDROID
void Android::RegisterController(jobject /*j_input_device*/)
{
}
#endif

void Android::SetButtonState(std::string guid, size_t port, int button_id, bool value)
{
    const auto identifier = GetIdentifier(guid, port);
    SetButton(identifier, button_id, value);
}

void Android::SetAxisPosition(std::string guid, size_t port, int axis_id, float value)
{
    const auto identifier = GetIdentifier(guid, port);
    SetAxis(identifier, axis_id, value);
}

void Android::SetMotionState(std::string guid, size_t port, u64 delta_timestamp, float gyro_x, float gyro_y, float gyro_z, float accel_x, float accel_y, float accel_z)
{
    const auto identifier = GetIdentifier(guid, port);
    const BasicMotion motion_data{
        .gyro_x = gyro_x,
        .gyro_y = gyro_y,
        .gyro_z = gyro_z,
        .accel_x = accel_x,
        .accel_y = accel_y,
        .accel_z = accel_z,
        .delta_timestamp = delta_timestamp,
    };
    SetMotion(identifier, 0, motion_data);
}

Common::Input::DriverResult Android::SetVibration([[maybe_unused]] const PadIdentifier & identifier, [[maybe_unused]] const Common::Input::VibrationStatus & vibration)
{
    vibration_queue.Push(VibrationRequest{
        .identifier = identifier,
        .vibration = vibration,
    });
    return Common::Input::DriverResult::Success;
}

bool Android::IsVibrationEnabled([[maybe_unused]] const PadIdentifier & identifier)
{
    return false;
}

Common::ParamPackage Android::BuildParamPackageForAnalog(PadIdentifier identifier, int axis_x, int axis_y) const
{
    Common::ParamPackage params;
    params.Set("engine", GetEngineName());
    params.Set("port", static_cast<int>(identifier.port));
    params.Set("guid", identifier.guid.RawString());
    params.Set("axis_x", axis_x);
    params.Set("axis_y", axis_y);
    params.Set("offset_x", 0);
    params.Set("offset_y", 0);
    params.Set("invert_x", "+");

    // Invert Y-Axis by default
    params.Set("invert_y", "-");
    return params;
}

Common::ParamPackage Android::BuildAnalogParamPackageForButton(PadIdentifier identifier, s32 axis, bool invert) const
{
    Common::ParamPackage params{};
    params.Set("engine", GetEngineName());
    params.Set("port", static_cast<int>(identifier.port));
    params.Set("guid", identifier.guid.RawString());
    params.Set("axis", axis);
    params.Set("threshold", "0.5");
    params.Set("invert", invert ? "-" : "+");
    return params;
}

Common::ParamPackage Android::BuildButtonParamPackageForButton(PadIdentifier identifier, s32 button) const
{
    Common::ParamPackage params{};
    params.Set("engine", GetEngineName());
    params.Set("port", static_cast<int>(identifier.port));
    params.Set("guid", identifier.guid.RawString());
    params.Set("button", button);
    return params;
}

bool Android::MatchVID(Common::UUID device, const std::vector<std::string> & vids) const
{
    for (size_t i = 0; i < vids.size(); ++i)
    {
        auto fucker = device.RawString();
        if (fucker.find(vids[i]) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

AnalogMapping Android::GetAnalogMappingForDevice(const IParamPackage & params)
{
    if (!params.Has("guid") || !params.Has("port"))
    {
        return {};
    }

#ifdef ANDROID
    auto identifier = GetIdentifier(params.GetString("guid", ""), static_cast<size_t>(params.GetInt("port", 0)));
    auto & j_device = input_devices[identifier];
    if (j_device == nullptr)
    {
        return {};
    }

    auto env = GetEnvForThread();
    std::set<s32> axes = GetDeviceAxes(env, j_device);
    if (axes.size() == 0)
    {
        return {};
    }

    AnalogMapping mapping = {};
    if (axes.find(AXIS_X) != axes.end() && axes.find(AXIS_Y) != axes.end())
    {
        mapping.insert_or_assign(NativeAnalogValues::LStick, BuildParamPackageForAnalog(identifier, AXIS_X, AXIS_Y));
    }

    if (axes.find(AXIS_RX) != axes.end() && axes.find(AXIS_RY) != axes.end())
    {
        mapping.insert_or_assign(NativeAnalogValues::RStick, BuildParamPackageForAnalog(identifier, AXIS_RX, AXIS_RY));
    }
    else if (axes.find(AXIS_Z) != axes.end() && axes.find(AXIS_RZ) != axes.end())
    {
        mapping.insert_or_assign(NativeAnalogValues::RStick, BuildParamPackageForAnalog(identifier, AXIS_Z, AXIS_RZ));
    }
    return mapping;
#else
    return {};
#endif
}

ButtonMapping Android::GetButtonMappingForDevice(const IParamPackage & params)
{
    if (!params.Has("guid") || !params.Has("port"))
    {
        return {};
    }

    return {};
}

ButtonNames Android::GetUIName([[maybe_unused]] const IParamPackage & params) const
{
    return ButtonNames::Value;
}

#ifdef ANDROID
std::set<s32> Android::GetDeviceAxes(JNIEnv * /*env*/, jobject & /*j_device*/) const
{
    return {};
}
#endif

std::vector<Common::ParamPackage> Android::GetInputDevices() const
{
    return {};
}

PadIdentifier Android::GetIdentifier(const std::string & guid, size_t port) const
{
    return {
        .guid = Common::UUID{guid},
        .port = port,
        .pad = 0,
    };
}

#ifdef ANDROID
void Android::SendVibrations(JNIEnv * /*env*/, std::stop_token token)
{
    vibration_queue.PopWait(token);
}
#endif

} // namespace InputCommon
