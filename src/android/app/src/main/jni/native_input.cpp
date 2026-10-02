#include "emulation_session.h"

#include <jni.h>

#include <nxemu-module-spec/operating_system.h>
#include <yuzu_common/fs/file.h>
#include <yuzu_common/fs/fs.h>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/interface_pointer.h>
#include <yuzu_common/interface_pointer_def.h>
#include <yuzu_common/param_package.h>

template class InterfacePtr<IParamPackageList>;

namespace
{
    IOperatingSystem * CurrentOS()
    {
        return EmulationSession::GetInstance().OperatingSystem();
    }

    std::filesystem::path InputProfileDir()
    {
        return Common::FS::GetYuzuPath(Common::FS::YuzuPath::ConfigDir) / "input";
    }
    
    void AddSupportedStyle(std::vector<int32_t> & supported, uint32_t set, NpadStyleSet bit, NpadStyleIndex index)
    {
        if ((set & (uint32_t)bit) != 0)
        {
            supported.push_back((int32_t)index);
        }
    }

    std::vector<int32_t> GetSupportedStyles(IOperatingSystem & os, int32_t player_index)
    {
        const uint32_t set = (uint32_t)os.GetSupportedStyleTag();
        std::vector<int32_t> supported;
        AddSupportedStyle(supported, set, NpadStyleSet::Fullkey, NpadStyleIndex::Fullkey);
        AddSupportedStyle(supported, set, NpadStyleSet::JoyDual, NpadStyleIndex::JoyconDual);
        AddSupportedStyle(supported, set, NpadStyleSet::JoyLeft, NpadStyleIndex::JoyconLeft);
        AddSupportedStyle(supported, set, NpadStyleSet::JoyRight, NpadStyleIndex::JoyconRight);
        if (player_index == 0)
        {
            AddSupportedStyle(supported, set, NpadStyleSet::Handheld, NpadStyleIndex::Handheld);
        }
        AddSupportedStyle(supported, set, NpadStyleSet::Gc, NpadStyleIndex::GameCube);
        return supported;
    }

    bool IsPlayerIndex(int32_t player_index)
    {
        return player_index >= 0 && player_index < 8;
    }

    IEmulatedController & ConfiguredController(IOperatingSystem & os, size_t player_index)
    {
        if (player_index == 0)
        {
            IEmulatedController & player_one = os.GetEmulatedController(NpadIdType::Player1);
            if (player_one.GetNpadStyleIndex(true) == NpadStyleIndex::Handheld)
            {
                return os.GetEmulatedController(NpadIdType::Handheld);
            }
            return player_one;
        }
        return os.GetEmulatedController((NpadIdType)player_index);
    }

    void SetControllerStyle(IOperatingSystem & os, size_t player_index, NpadStyleIndex style)
    {
        if (player_index == 0)
        {
            IEmulatedController & handheld = os.GetEmulatedController(NpadIdType::Handheld);
            IEmulatedController & player_one = os.GetEmulatedController(NpadIdType::Player1);
            const bool connected = handheld.IsConnected(true) || player_one.IsConnected(true);
            handheld.SetNpadStyleIndex(style);
            player_one.SetNpadStyleIndex(style);
            if (connected)
            {
                if (style == NpadStyleIndex::Handheld)
                {
                    player_one.Disconnect();
                    handheld.Connect(true);
                }
                else
                {
                    handheld.Disconnect();
                    player_one.Connect(true);
                }
            }
            handheld.SaveCurrentConfig();
            player_one.SaveCurrentConfig();
            return;
        }

        IEmulatedController & controller = os.GetEmulatedController((NpadIdType)player_index);
        controller.SetNpadStyleIndex(style);
        controller.SaveCurrentConfig();
    }

    void EnsureSupportedStyle(IOperatingSystem & os, int player_index, IEmulatedController & controller)
    {
        const std::vector<s32> supported = GetSupportedStyles(os, player_index);
        const s32 style = (s32)controller.GetNpadStyleIndex(true);
        if (!supported.empty() &&
            std::find(supported.begin(), supported.end(), style) == supported.end())
        {
            controller.SetNpadStyleIndex((NpadStyleIndex)supported[0]);
        }
    }

    void ConnectController(SystemModules & modules, size_t player_index, bool connected)
    {
        IOperatingSystem & os = modules.Modules().OperatingSystem();
        if (player_index == 0)
        {
            IEmulatedController & handheld = os.GetEmulatedController(NpadIdType::Handheld);
            IEmulatedController & player_one = os.GetEmulatedController(NpadIdType::Player1);
            EnsureSupportedStyle(os, 0, handheld);
            EnsureSupportedStyle(os, 0, player_one);
            if (player_one.GetNpadStyleIndex(true) == NpadStyleIndex::Handheld)
            {
                if (connected)
                {
                    handheld.Connect(true);
                }
                else
                {
                    handheld.Disconnect();
                }
                player_one.Disconnect();
            }
            else
            {
                if (connected)
                {
                    player_one.Connect(true);
                }
                else
                {
                    player_one.Disconnect();
                }
                handheld.Disconnect();
            }
            handheld.SaveCurrentConfig();
            player_one.SaveCurrentConfig();
        }
        else
        {
            IEmulatedController & controller = os.GetEmulatedController((NpadIdType)player_index);
            EnsureSupportedStyle(os, (int)player_index, controller);
            if (connected)
            {
                controller.Connect(true);
            }
            else
            {
                controller.Disconnect();
            }
            controller.SaveCurrentConfig();
        }
    }

    jobjectArray ToStringArray(JNIEnv * env, const std::vector<std::string> & values)
    {
        jclass string_class = env->FindClass("java/lang/String");
        jobjectArray array = env->NewObjectArray((jsize)values.size(), string_class, env->NewStringUTF(""));
        for (size_t i = 0; i < values.size(); ++i)
        {
            env->SetObjectArrayElement(array, (jsize)i, env->NewStringUTF(values[i].c_str()));
        }
        env->DeleteLocalRef(string_class);
        return array;
    }
}

extern "C" {

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_onGamePadButtonEvent(JNIEnv * env, jobject /*obj*/, jstring j_guid, jint j_port, jint j_button_id, jint j_action)
{
    IOperatingSystem * os = CurrentOS();
    if (os == nullptr || j_guid == nullptr)
    {
        return;
    }
    const char * guid = env->GetStringUTFChars(j_guid, nullptr);
    os->SetButtonState(guid, j_port, j_button_id, j_action != 0);
    env->ReleaseStringUTFChars(j_guid, guid);
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_onGamePadAxisEvent(JNIEnv * env, jobject /*obj*/, jstring j_guid, jint j_port, jint j_axis_id, jfloat j_value)
{
    IOperatingSystem * os = CurrentOS();
    if (os == nullptr || j_guid == nullptr)
    {
        return;
    }
    const char * guid = env->GetStringUTFChars(j_guid, nullptr);
    os->SetAxisPosition(guid, j_port, j_axis_id, j_value);
    env->ReleaseStringUTFChars(j_guid, guid);
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_registerController(JNIEnv * /*env*/, jobject /*obj*/, jobject j_device)
{
    IOperatingSystem * os = CurrentOS();
    if (os != nullptr && j_device != nullptr)
    {
        os->AndroidRegisterController(j_device);
    }
}

JNIEXPORT jobjectArray JNICALL Java_org_nxemu_features_input_NativeInput_getInputDevices(JNIEnv * env, jobject /*obj*/)
{
    IOperatingSystem * os = CurrentOS();
    std::vector<std::string> devices;
    if (os != nullptr)
    {
        InterfacePtr<IParamPackageList> list(os->GetInputDevices());
        if (list)
        {
            for (uint32_t i = 0; i < list->GetCount(); ++i)
            {
                devices.emplace_back(list->GetParamPackage(i).Serialize());
            }
        }
    }
    return ToStringArray(env, devices);
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_loadInputProfiles(JNIEnv * /*env*/, jobject /*obj*/)
{
}

JNIEXPORT jobjectArray JNICALL Java_org_nxemu_features_input_NativeInput_getInputProfileNames(JNIEnv * env, jobject /*obj*/)
{
    std::vector<std::string> names;
    const auto dir = InputProfileDir();
    if (Common::FS::IsDir(dir))
    {
        Common::FS::IterateDirEntries(
            dir,
            [&](const std::filesystem::directory_entry & entry) {
                const auto & full_path = entry.path();
                if (full_path.extension() == ".json")
                {
                    names.push_back(full_path.stem().string());
                }
                return true;
            },
            Common::FS::DirEntryFilter::File);
        std::sort(names.begin(), names.end());
    }
    return ToStringArray(env, names);
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_beginMapping(JNIEnv * /*env*/,
                                                                              jobject /*obj*/,
                                                                              jint jtype)
{
    IOperatingSystem * os = CurrentOS();
    if (os != nullptr)
    {
        os->BeginMapping(static_cast<PollingInputType>(jtype));
    }
}

JNIEXPORT jstring JNICALL Java_org_nxemu_features_input_NativeInput_getNextInput(JNIEnv * env, jobject /*obj*/)
{
    IOperatingSystem * os = CurrentOS();
    if (os == nullptr)
    {
        return env->NewStringUTF("[empty]");
    }
    IParamPackage * next = os->GetNextInput();
    const char * serialized = next != nullptr ? next->Serialize() : "[empty]";
    jstring result = env->NewStringUTF(serialized);
    if (next != nullptr)
    {
        next->Release();
    }
    return result;
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_stopMapping(JNIEnv * /*env*/,
                                                                             jobject /*obj*/)
{
    IOperatingSystem * os = CurrentOS();
    if (os != nullptr)
    {
        os->StopMapping();
    }
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_updateMappingsWithDefaultImpl(JNIEnv * env, jobject /*obj*/, jint j_player_index, jstring j_device_params, jstring j_display_name)
{
    SystemModules * modules = EmulationSession::GetInstance().Modules();
    if (modules == nullptr || !IsPlayerIndex(j_player_index) || j_device_params == nullptr)
    {
        return;
    }
    IOperatingSystem & os = modules->Modules().OperatingSystem();
    const char * params_chars = env->GetStringUTFChars(j_device_params, nullptr);
    const char * display_chars =
        j_display_name != nullptr ? env->GetStringUTFChars(j_display_name, nullptr) : "";
    IParamPackageImpl device{Common::ParamPackage{params_chars}};
    env->ReleaseStringUTFChars(j_device_params, params_chars);

    IEmulatedController & controller = ConfiguredController(os, (size_t)j_player_index);
    IParamPackageImpl empty{Common::ParamPackage{}};
    for (uint32_t button_id = 0; button_id < (uint32_t)NativeButtonValues::NumButtons; ++button_id)
    {
        controller.SetButtonParam(button_id, empty);
    }
    for (uint32_t analog_id = 0; analog_id < (uint32_t)NativeAnalogValues::NumAnalogs; ++analog_id)
    {
        controller.SetStickParam(analog_id, empty);
    }

    IButtonMappingList * buttons = os.GetButtonMappingForDevice(device);
    if (buttons != nullptr)
    {
        for (uint32_t i = 0; i < buttons->GetCount(); ++i)
        {
            Common::ParamPackage named(buttons->GetParamPackage(i));
            named.Set("display", display_chars);
            IParamPackageImpl named_pkg{named};
            controller.SetButtonParam(buttons->GetIndex(i), named_pkg);
        }
        buttons->Release();
    }

    IButtonMappingList * analogs = os.GetAnalogMappingForDevice(device);
    if (analogs != nullptr)
    {
        for (uint32_t i = 0; i < analogs->GetCount(); ++i)
        {
            Common::ParamPackage named(analogs->GetParamPackage(i));
            named.Set("display", display_chars);
            IParamPackageImpl named_pkg{named};
            controller.SetStickParam(analogs->GetIndex(i), named_pkg);
        }
        analogs->Release();
    }

    if (j_display_name != nullptr)
    {
        env->ReleaseStringUTFChars(j_display_name, display_chars);
    }
    controller.SaveCurrentConfig();
    modules->FlushSettings();
}

JNIEXPORT jstring JNICALL Java_org_nxemu_features_input_NativeInput_getButtonParamImpl(JNIEnv * env, jobject /*obj*/, jint j_player_index, jint j_button)
{
    IOperatingSystem * os = CurrentOS();
    if (os == nullptr || !IsPlayerIndex(j_player_index))
    {
        return env->NewStringUTF("[empty]");
    }
    IParamPackage * param = ConfiguredController(*os, (size_t)j_player_index).GetButtonParamPtr((uint32_t)j_button);
    const char * serialized = param != nullptr ? param->Serialize() : "[empty]";
    jstring result = env->NewStringUTF(serialized);
    if (param != nullptr)
    {
        param->Release();
    }
    return result;
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_setButtonParamImpl(
    JNIEnv * env, jobject /*obj*/, jint j_player_index, jint j_button_id, jstring j_param)
{
    SystemModules * modules = EmulationSession::GetInstance().Modules();
    if (modules == nullptr || !IsPlayerIndex(j_player_index) || j_param == nullptr)
    {
        return;
    }
    const char * serialized = env->GetStringUTFChars(j_param, nullptr);
    IParamPackageImpl pkg{Common::ParamPackage{serialized}};
    env->ReleaseStringUTFChars(j_param, serialized);
    IEmulatedController & controller =
        ConfiguredController(modules->Modules().OperatingSystem(), static_cast<size_t>(j_player_index));
    controller.SetButtonParam(static_cast<uint32_t>(j_button_id), pkg);
    controller.SaveCurrentConfig();
    modules->FlushSettings();
}

JNIEXPORT jstring JNICALL Java_org_nxemu_features_input_NativeInput_getStickParamImpl(JNIEnv * env, jobject /*obj*/, jint j_player_index, jint j_stick)
{
    IOperatingSystem * os = CurrentOS();
    if (os == nullptr || !IsPlayerIndex(j_player_index))
    {
        return env->NewStringUTF("[empty]");
    }
    IParamPackage * param = ConfiguredController(*os, (size_t)j_player_index).GetStickParamPtr((uint32_t)j_stick);
    const char * serialized = param != nullptr ? param->Serialize() : "[empty]";
    jstring result = env->NewStringUTF(serialized);
    if (param != nullptr)
    {
        param->Release();
    }
    return result;
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_setStickParamImpl(
    JNIEnv * env, jobject /*obj*/, jint j_player_index, jint j_stick_id, jstring j_param)
{
    SystemModules * modules = EmulationSession::GetInstance().Modules();
    if (modules == nullptr || !IsPlayerIndex(j_player_index) || j_param == nullptr)
    {
        return;
    }
    const char * serialized = env->GetStringUTFChars(j_param, nullptr);
    IParamPackageImpl pkg{Common::ParamPackage{serialized}};
    env->ReleaseStringUTFChars(j_param, serialized);
    IEmulatedController & controller =
        ConfiguredController(modules->Modules().OperatingSystem(), static_cast<size_t>(j_player_index));
    controller.SetStickParam(static_cast<uint32_t>(j_stick_id), pkg);
    controller.SaveCurrentConfig();
    modules->FlushSettings();
}

JNIEXPORT jint JNICALL Java_org_nxemu_features_input_NativeInput_getButtonNameImpl(JNIEnv * env, jobject /*obj*/, jstring j_param)
{
    IOperatingSystem * os = CurrentOS();
    if (os == nullptr || j_param == nullptr)
    {
        return (jint)ButtonNames::Invalid;
    }
    const char * serialized = env->GetStringUTFChars(j_param, nullptr);
    IParamPackageImpl pkg{Common::ParamPackage{serialized}};
    env->ReleaseStringUTFChars(j_param, serialized);
    return (jint)os->GetButtonName(pkg);
}

JNIEXPORT jintArray JNICALL Java_org_nxemu_features_input_NativeInput_getSupportedStyleTagsImpl(JNIEnv * env, jobject /*obj*/, jint j_player_index)
{
    IOperatingSystem * os = CurrentOS();
    std::vector<s32> supported;
    if (os != nullptr)
    {
        supported = GetSupportedStyles(*os, j_player_index);
    }
    jintArray result = env->NewIntArray((jsize)supported.size());
    if (!supported.empty())
    {
        env->SetIntArrayRegion(result, 0, (jsize)supported.size(), supported.data());
    }
    return result;
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_setStyleIndexImpl(JNIEnv * /*env*/, jobject /*obj*/, jint j_player_index, jint j_style_index)
{
    SystemModules * modules = EmulationSession::GetInstance().Modules();
    if (modules == nullptr || !IsPlayerIndex(j_player_index))
    {
        return;
    }
    SetControllerStyle(modules->Modules().OperatingSystem(), (size_t)j_player_index, (NpadStyleIndex)j_style_index);
    modules->FlushSettings();
}

JNIEXPORT jboolean JNICALL Java_org_nxemu_features_input_NativeInput_isControllerImpl(JNIEnv * env, jobject /*obj*/, jstring j_params)
{
    IOperatingSystem * os = CurrentOS();
    if (os == nullptr || j_params == nullptr)
    {
        return JNI_FALSE;
    }
    const char * serialized = env->GetStringUTFChars(j_params, nullptr);
    IParamPackageImpl pkg{Common::ParamPackage{serialized}};
    env->ReleaseStringUTFChars(j_params, serialized);
    return os->IsController(pkg) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_nxemu_features_input_NativeInput_getIsConnected(JNIEnv * /*env*/, jobject /*obj*/, jint j_player_index)
{
    IOperatingSystem * os = CurrentOS();
    if (os == nullptr)
    {
        return JNI_FALSE;
    }
    return os->GetEmulatedController((NpadIdType)j_player_index).IsConnected(true) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_connectControllerImpl(JNIEnv * /*env*/, jobject /*obj*/, jint j_player_index, jboolean j_connected)
{
    SystemModules * modules = EmulationSession::GetInstance().Modules();
    if (modules == nullptr || !IsPlayerIndex(j_player_index))
    {
        return;
    }
    ConnectController(*modules, (size_t)j_player_index, j_connected == JNI_TRUE);
    modules->FlushSettings();
}

JNIEXPORT void JNICALL Java_org_nxemu_features_input_NativeInput_resetControllerMappings(JNIEnv * /*env*/, jobject /*obj*/, jint j_player_index)
{
    SystemModules * modules = EmulationSession::GetInstance().Modules();
    if (modules == nullptr || !IsPlayerIndex(j_player_index))
    {
        return;
    }
    IOperatingSystem & os = modules->Modules().OperatingSystem();
    const size_t player_index = static_cast<size_t>(j_player_index);
    SetControllerStyle(os, player_index, NpadStyleIndex::Fullkey);
    ConnectController(*modules, player_index, player_index == 0);
    IEmulatedController & controller = ConfiguredController(os, player_index);
    IParamPackageImpl empty{Common::ParamPackage{}};
    for (uint32_t i = 0; i < (uint32_t)NativeButtonValues::NumButtons; ++i)
    {
        controller.SetButtonParam(i, empty);
    }
    for (uint32_t i = 0; i < (uint32_t)NativeAnalogValues::NumAnalogs; ++i)
    {
        controller.SetStickParam(i, empty);
    }
    controller.SaveCurrentConfig();
    modules->FlushSettings();
}

} // extern "C"
