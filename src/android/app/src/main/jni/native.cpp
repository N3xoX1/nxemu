#include <android/log.h>
#include <jni.h>
#include <string>

#include <nxemu-core/app_init.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/version.h>
#include <yuzu_common/android/java_bridge.h>
#include <yuzu_common/fs/fs_android.h>
#include <yuzu_common/fs/path_util.h>
#include "emulation_session.h"
#include "notification.h"
#include "settings/settings_bridge.h"

namespace
{
    constexpr const char * kLogTag = "NxEmu";

    jclass g_native_library_class = nullptr;

    std::string JStringToUtf8(JNIEnv * env, jstring value)
    {
        if (value == nullptr)
        {
            return {};
        }
        const char * chars = env->GetStringUTFChars(value, nullptr);
        std::string result = chars != nullptr ? chars : "";
        if (chars != nullptr)
        {
            env->ReleaseStringUTFChars(value, chars);
        }
        return result;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_nxemu_NativeLibrary_appInit(JNIEnv * env, jclass /*clazz*/, jstring appDirectory,
                                     jstring nativeModuleLibDir)
{
    JavaVM * vm = nullptr;
    env->GetJavaVM(&vm);
    SetJavaVM(vm);

    if (g_native_library_class != nullptr)
    {
        env->DeleteGlobalRef(g_native_library_class);
        g_native_library_class = nullptr;
    }

    jclass local_class = env->FindClass("org/nxemu/NativeLibrary");
    g_native_library_class = static_cast<jclass>(env->NewGlobalRef(local_class));
    SetNativeLibraryGlobalRef(env->NewGlobalRef(local_class));
    env->DeleteLocalRef(local_class);

    const std::string baseDir = JStringToUtf8(env, appDirectory);
    const std::string moduleDir = JStringToUtf8(env, nativeModuleLibDir);
    Common::FS::SetAppDirectory(std::string(baseDir));

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "_____     _____            ");
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "|   | |_ _|   __|_____ _ _ ");
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "| | | |_'_|   __|     | | |");
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "|_|___|_,_|_____|_|_|_|___|");
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "");
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "https://www.nxemu.com/");
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s Version %s", VER_FILE_DESCRIPTION_STR, VER_FILE_VERSION_STR);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "baseDir: %s", baseDir.c_str());
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "moduleDir: %s", moduleDir.c_str());

    if (!AppInit(&AndroidNotification::GetInstance(), moduleDir.c_str(), baseDir.c_str()))
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "AppInit failed (app=%s module=%s)", baseDir.c_str(), moduleDir.c_str());
        return;
    }

    SettingsStore & store = SettingsStore::GetInstance();
    store.SetString(NXCoreSetting::ModuleDirectory, moduleDir.c_str());
    SettingsChange_Start(vm, env);
    EmulationSession::GetInstance().InitializeSystem();
}

extern "C" JNIEXPORT void JNICALL
Java_org_nxemu_NativeLibrary_appCleanup(JNIEnv * env, jclass /*clazz*/)
{
    EmulationSession::GetInstance().ShutdownSystem();
    SettingsChange_Stop(env);
    AppCleanup();
    Common::FS::Android::UnRegisterCallbacks(env);
    ClearNativeLibraryGlobalRef(env);
    if (g_native_library_class != nullptr)
    {
        env->DeleteGlobalRef(g_native_library_class);
        g_native_library_class = nullptr;
    }
}
