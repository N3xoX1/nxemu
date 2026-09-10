#include "settings_bridge.h"
#include "ui_identifiers.h"
#include "ui_settings.h"
#include <android/log.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <yuzu_common/android/java_bridge.h>

static jmethodID s_on_setting_changed = nullptr;

namespace
{
    constexpr const char * kLogTag = "NxEmu";

    jclass s_native_library_class = nullptr;

    void OnSettingChanged(const char * setting, void * /*userData*/)
    {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "OnSettingChanged setting: %s", setting);
        if (s_native_library_class == nullptr || s_on_setting_changed == nullptr || setting == nullptr)
        {
            return;
        }

        JNIEnv * env = GetEnvForThread();
        if (env == nullptr)
        {
            return;
        }

        jstring key = env->NewStringUTF(setting);
        env->CallStaticVoidMethod(s_native_library_class, s_on_setting_changed, key);
        env->DeleteLocalRef(key);
        if (env->ExceptionCheck())
        {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }
}

extern "C" JNIEXPORT jstring JNICALL Java_org_nxemu_NativeLibrary_getSettingString(JNIEnv * env, jclass /*clazz*/, jstring setting)
{
    const char * key = env->GetStringUTFChars(setting, nullptr);
    const char * value = SettingsStore::GetInstance().GetString(key);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Get Setting key: %s", key);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Get Setting value: %s", value);
    env->ReleaseStringUTFChars(setting, key);
    return env->NewStringUTF(value != nullptr ? value : "");
}

extern "C" JNIEXPORT jboolean JNICALL Java_org_nxemu_NativeLibrary_getSettingBool(JNIEnv * env, jclass /*clazz*/, jstring setting)
{
    if (setting == nullptr)
    {
        return JNI_FALSE;
    }
    const char * key = env->GetStringUTFChars(setting, nullptr);
    const bool value = SettingsStore::GetInstance().GetBool(key);
    env->ReleaseStringUTFChars(setting, key);
    return value ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL Java_org_nxemu_NativeLibrary_setSettingString(JNIEnv * env, jclass /*clazz*/, jstring setting, jstring value)
{
    const char * key = env->GetStringUTFChars(setting, nullptr);
    const char * val = env->GetStringUTFChars(value, nullptr);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Set Setting key: %s", key);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Set Setting value: %s", val);
    SettingsStore::GetInstance().SetString(key, val);
    env->ReleaseStringUTFChars(value, val);
    env->ReleaseStringUTFChars(setting, key);
}

extern "C" JNIEXPORT void JNICALL Java_org_nxemu_NativeLibrary_saveSettings(JNIEnv * /*env*/, jclass /*clazz*/)
{
    SaveUISetting();
    SettingsStore::GetInstance().Save();
}

void SettingsChange_Start(JavaVM * /*javaVm*/, JNIEnv * env)
{
    const jclass cls = env->FindClass("org/nxemu/NativeLibrary");
    s_native_library_class = reinterpret_cast<jclass>(env->NewGlobalRef(cls));
    s_on_setting_changed = env->GetStaticMethodID(cls, "onSettingChanged", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(cls);

    SettingsStore & store = SettingsStore::GetInstance();
    store.RegisterCallback(NXUISetting::GameDirectories, OnSettingChanged, nullptr);
    store.RegisterCallback(NXCoreSetting::DisplayedFrames, OnSettingChanged, nullptr);
}

void SettingsChange_Stop(JNIEnv * env)
{
    SettingsStore & store = SettingsStore::GetInstance();
    store.UnregisterCallback(NXUISetting::GameDirectories, OnSettingChanged, nullptr);
    store.UnregisterCallback(NXCoreSetting::DisplayedFrames, OnSettingChanged, nullptr);

    s_on_setting_changed = nullptr;
    if (s_native_library_class != nullptr)
    {
        env->DeleteGlobalRef(s_native_library_class);
        s_native_library_class = nullptr;
    }
}
