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
        JNIEnv * env = GetEnvForThread();
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
void Android::RegisterController(jobject j_input_device)
{
    if (j_input_device == nullptr || GetNxInputDeviceGetGUID() == nullptr)
    {
        return;
    }
    JNIEnv * env = GetEnvForThread();
    if (env == nullptr)
    {
        return;
    }
    const std::string guid = GetJString(env, static_cast<jstring>(env->CallObjectMethod(j_input_device, GetNxInputDeviceGetGUID())));
    const s32 port = env->CallIntMethod(j_input_device, GetNxInputDeviceGetPort());
    const PadIdentifier identifier = GetIdentifier(guid, static_cast<size_t>(port));
    PreSetController(identifier);

    if (input_devices.find(identifier) != input_devices.end())
    {
        env->DeleteGlobalRef(input_devices[identifier]);
    }
    input_devices[identifier] = env->NewGlobalRef(j_input_device);
}
#endif

void Android::SetButtonState(std::string guid, size_t port, int button_id, bool value)
{
    const PadIdentifier identifier = GetIdentifier(guid, port);
    PreSetController(identifier);
    SetButton(identifier, button_id, value);
}

void Android::SetAxisPosition(std::string guid, size_t port, int axis_id, float value)
{
    const PadIdentifier identifier = GetIdentifier(guid, port);
    PreSetController(identifier);
    SetAxis(identifier, axis_id, value);
}

void Android::SetMotionState(std::string guid, size_t port, u64 delta_timestamp, float gyro_x, float gyro_y, float gyro_z, float accel_x, float accel_y, float accel_z)
{
    const PadIdentifier identifier = GetIdentifier(guid, port);
    PreSetController(identifier);
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
#ifdef ANDROID
    std::unordered_map<PadIdentifier, jobject>::iterator device = input_devices.find(identifier);
    if (device != input_devices.end() && GetNxInputDeviceGetSupportsVibration() != nullptr)
    {
        return RunJNIOnFiber<bool>([&](JNIEnv * env) {
            return env->CallBooleanMethod(device->second, GetNxInputDeviceGetSupportsVibration()) !=
                   JNI_FALSE;
        });
    }
#endif
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
        const std::string fucker = device.RawString();
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
    const PadIdentifier identifier = GetIdentifier(params.GetString("guid", ""), static_cast<size_t>(params.GetInt("port", 0)));
    jobject & j_device = input_devices[identifier];
    if (j_device == nullptr)
    {
        return {};
    }

    JNIEnv * env = GetEnvForThread();
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

#ifdef ANDROID
    const PadIdentifier identifier =
        GetIdentifier(params.GetString("guid", ""), static_cast<size_t>(params.GetInt("port", 0)));
    jobject & j_device = input_devices[identifier];
    if (j_device == nullptr || GetNxInputDeviceHasKeys() == nullptr)
    {
        return {};
    }

    JNIEnv * env = GetEnvForThread();
    if (env == nullptr)
    {
        return {};
    }
    jintArray j_keys = env->NewIntArray(static_cast<int>(keycode_ids.size()));
    env->SetIntArrayRegion(j_keys, 0, static_cast<int>(keycode_ids.size()), keycode_ids.data());
    jbooleanArray j_has_keys_object = static_cast<jbooleanArray>(
        env->CallObjectMethod(j_device, GetNxInputDeviceHasKeys(), j_keys));
    env->DeleteLocalRef(j_keys);
    if (j_has_keys_object == nullptr)
    {
        return {};
    }
    jboolean isCopy = JNI_FALSE;
    jboolean * j_has_keys = env->GetBooleanArrayElements(j_has_keys_object, &isCopy);

    std::set<s32> available_keys;
    for (size_t i = 0; i < keycode_ids.size(); ++i)
    {
        if (j_has_keys[i])
        {
            available_keys.insert(keycode_ids[i]);
        }
    }
    env->ReleaseBooleanArrayElements(j_has_keys_object, j_has_keys, JNI_ABORT);
    env->DeleteLocalRef(j_has_keys_object);

    std::set<s32> axes = GetDeviceAxes(env, j_device);

    ButtonMapping mapping = {};
    if (axes.find(AXIS_HAT_X) != axes.end() && axes.find(AXIS_HAT_Y) != axes.end())
    {
        mapping.insert_or_assign(NativeButtonValues::DUp,
                                 BuildAnalogParamPackageForButton(identifier, AXIS_HAT_Y, true));
        mapping.insert_or_assign(NativeButtonValues::DDown,
                                 BuildAnalogParamPackageForButton(identifier, AXIS_HAT_Y, false));
        mapping.insert_or_assign(NativeButtonValues::DLeft,
                                 BuildAnalogParamPackageForButton(identifier, AXIS_HAT_X, true));
        mapping.insert_or_assign(NativeButtonValues::DRight,
                                 BuildAnalogParamPackageForButton(identifier, AXIS_HAT_X, false));
    }
    else if (available_keys.find(KEYCODE_DPAD_UP) != available_keys.end() &&
             available_keys.find(KEYCODE_DPAD_DOWN) != available_keys.end() &&
             available_keys.find(KEYCODE_DPAD_LEFT) != available_keys.end() &&
             available_keys.find(KEYCODE_DPAD_RIGHT) != available_keys.end())
    {
        mapping.insert_or_assign(NativeButtonValues::DUp,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_DPAD_UP));
        mapping.insert_or_assign(NativeButtonValues::DDown,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_DPAD_DOWN));
        mapping.insert_or_assign(NativeButtonValues::DLeft,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_DPAD_LEFT));
        mapping.insert_or_assign(NativeButtonValues::DRight,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_DPAD_RIGHT));
    }

    if (axes.find(AXIS_LTRIGGER) != axes.end())
    {
        mapping.insert_or_assign(NativeButtonValues::ZL,
                                 BuildAnalogParamPackageForButton(identifier, AXIS_LTRIGGER, false));
    }
    else if (available_keys.find(KEYCODE_BUTTON_L2) != available_keys.end())
    {
        mapping.insert_or_assign(NativeButtonValues::ZL,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_L2));
    }

    if (axes.find(AXIS_RTRIGGER) != axes.end())
    {
        mapping.insert_or_assign(NativeButtonValues::ZR,
                                 BuildAnalogParamPackageForButton(identifier, AXIS_RTRIGGER, false));
    }
    else if (available_keys.find(KEYCODE_BUTTON_R2) != available_keys.end())
    {
        mapping.insert_or_assign(NativeButtonValues::ZR,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_R2));
    }

    if (available_keys.find(KEYCODE_BUTTON_A) != available_keys.end())
    {
        mapping.insert_or_assign(
            MatchVID(identifier.guid, flipped_ab_vids) ? NativeButtonValues::B : NativeButtonValues::A,
            BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_A));
    }
    if (available_keys.find(KEYCODE_BUTTON_B) != available_keys.end())
    {
        mapping.insert_or_assign(
            MatchVID(identifier.guid, flipped_ab_vids) ? NativeButtonValues::A : NativeButtonValues::B,
            BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_B));
    }
    if (available_keys.find(KEYCODE_BUTTON_X) != available_keys.end())
    {
        mapping.insert_or_assign(
            MatchVID(identifier.guid, flipped_xy_vids) ? NativeButtonValues::Y : NativeButtonValues::X,
            BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_X));
    }
    if (available_keys.find(KEYCODE_BUTTON_Y) != available_keys.end())
    {
        mapping.insert_or_assign(
            MatchVID(identifier.guid, flipped_xy_vids) ? NativeButtonValues::X : NativeButtonValues::Y,
            BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_Y));
    }

    if (available_keys.find(KEYCODE_BUTTON_L1) != available_keys.end())
    {
        mapping.insert_or_assign(NativeButtonValues::L,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_L1));
    }
    if (available_keys.find(KEYCODE_BUTTON_R1) != available_keys.end())
    {
        mapping.insert_or_assign(NativeButtonValues::R,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_R1));
    }
    if (available_keys.find(KEYCODE_BUTTON_THUMBL) != available_keys.end())
    {
        mapping.insert_or_assign(NativeButtonValues::LStick,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_THUMBL));
    }
    if (available_keys.find(KEYCODE_BUTTON_THUMBR) != available_keys.end())
    {
        mapping.insert_or_assign(NativeButtonValues::RStick,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_THUMBR));
    }
    if (available_keys.find(KEYCODE_BUTTON_START) != available_keys.end())
    {
        mapping.insert_or_assign(NativeButtonValues::Plus,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_START));
    }
    if (available_keys.find(KEYCODE_BUTTON_SELECT) != available_keys.end())
    {
        mapping.insert_or_assign(NativeButtonValues::Minus,
                                 BuildButtonParamPackageForButton(identifier, KEYCODE_BUTTON_SELECT));
    }
    return mapping;
#else
    return {};
#endif
}

ButtonNames Android::GetUIName([[maybe_unused]] const IParamPackage & params) const
{
    return ButtonNames::Value;
}

#ifdef ANDROID
std::set<s32> Android::GetDeviceAxes(JNIEnv * env, jobject & j_device) const
{
    std::set<s32> axes;
    if (env == nullptr || j_device == nullptr || GetNxInputDeviceGetAxes() == nullptr ||
        GetIntegerIntValue() == nullptr)
    {
        return axes;
    }
    jobjectArray j_axes =
        static_cast<jobjectArray>(env->CallObjectMethod(j_device, GetNxInputDeviceGetAxes()));
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
    }
    if (j_axes == nullptr)
    {
        return axes;
    }
    const jsize count = env->GetArrayLength(j_axes);
    for (jsize i = 0; i < count; ++i)
    {
        jobject axis = env->GetObjectArrayElement(j_axes, i);
        if (axis != nullptr)
        {
            axes.insert(env->CallIntMethod(axis, GetIntegerIntValue()));
            if (env->ExceptionCheck())
            {
                env->ExceptionClear();
            }
            env->DeleteLocalRef(axis);
        }
    }
    env->DeleteLocalRef(j_axes);
    return axes;
}
#endif

std::vector<Common::ParamPackage> Android::GetInputDevices() const
{
#ifdef ANDROID
    std::vector<Common::ParamPackage> devices;
    JNIEnv * env = GetEnvForThread();
    if (env == nullptr || GetNxInputDeviceGetName() == nullptr)
    {
        return devices;
    }
    for (const std::pair<const PadIdentifier, jobject> & device : input_devices)
    {
        const PadIdentifier & key = device.first;
        const jobject value = device.second;
        jstring name_object =
            static_cast<jstring>(env->CallObjectMethod(value, GetNxInputDeviceGetName()));
        std::string name = GetJString(env, name_object) + " " + std::to_string(key.port);
        if (name_object != nullptr)
        {
            env->DeleteLocalRef(name_object);
        }
        devices.emplace_back(Common::ParamPackage{
            {"engine", GetEngineName()},
            {"display", std::move(name)},
            {"guid", key.guid.RawString()},
            {"port", std::to_string(key.port)},
        });
    }
    return devices;
#else
    return {};
#endif
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
void Android::SendVibrations(JNIEnv * env, std::stop_token token)
{
    VibrationRequest request = vibration_queue.PopWait(token);
    if (env == nullptr || GetNxInputDeviceVibrate() == nullptr)
    {
        return;
    }
    std::unordered_map<PadIdentifier, jobject>::iterator device = input_devices.find(request.identifier);
    if (device != input_devices.end())
    {
        const float average_intensity = static_cast<float>(
            (request.vibration.high_amplitude + request.vibration.low_amplitude) / 2.0);
        env->CallVoidMethod(device->second, GetNxInputDeviceVibrate(), average_intensity);
    }
}
#endif

} // namespace InputCommon
