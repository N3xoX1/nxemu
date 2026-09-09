#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <string>

#include <common/json.h>
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

    std::string FailJson(const std::string & path, const char * err)
    {
        JsonValue obj(JsonValueType::Object);
        obj["path"] = path;
        obj["title"] = "";
        obj["programId"] = "0";
        obj["fileType"] = 0;
        obj["icon"] = "";
        obj["error"] = err != nullptr ? err : "";
        return JsonStyledWriter().write(obj);
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

extern "C" JNIEXPORT jstring JNICALL
Java_org_nxemu_NativeLibrary_queryRomMetadata(JNIEnv * env, jclass /*clazz*/, jstring j_path)
{
    if (j_path == nullptr)
    {
        const std::string json = FailJson({}, "null_path");
        return env->NewStringUTF(json.c_str());
    }

    const std::string path = JStringToUtf8(env, j_path);
    const std::string json = EmulationSession::GetInstance().QueryRomMetadata(path);
    if (json.empty())
    {
        const std::string fail = FailJson(path, "metadata_unavailable");
        return env->NewStringUTF(fail.c_str());
    }
    return env->NewStringUTF(json.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_nxemu_NativeLibrary_emulationSurfaceReady(JNIEnv * env, jclass /*clazz*/, jobject jsurface,
                                                   jfloat pixel_ratio, jstring jpath)
{
    if (jsurface == nullptr || jpath == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "emulationSurfaceReady: null surface or path");
        return JNI_FALSE;
    }

    ANativeWindow * nw = ANativeWindow_fromSurface(env, jsurface);
    if (nw == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "emulationSurfaceReady: ANativeWindow_fromSurface failed");
        return JNI_FALSE;
    }

    const std::string rom_path = JStringToUtf8(env, jpath);
    if (rom_path.empty())
    {
        ANativeWindow_release(nw);
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "emulationSurfaceReady: empty path");
        return JNI_FALSE;
    }

    return EmulationSession::GetInstance().Run(nw, pixel_ratio, rom_path) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_nxemu_NativeLibrary_emulationSurfaceDestroyed(JNIEnv * /*env*/, jclass /*clazz*/)
{
    EmulationSession::GetInstance().SurfaceDestroyed();
}

extern "C" JNIEXPORT void JNICALL
Java_org_nxemu_NativeLibrary_surfaceChanged(JNIEnv * env, jclass /*clazz*/, jobject jsurface)
{
    if (jsurface == nullptr)
    {
        return;
    }
    ANativeWindow * nw = ANativeWindow_fromSurface(env, jsurface);
    if (nw == nullptr)
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "surfaceChanged: ANativeWindow_fromSurface failed");
        return;
    }
    EmulationSession & session = EmulationSession::GetInstance();
    if (session.NativeWindow() == nw)
    {
        ANativeWindow_release(nw);
        session.SurfaceChanged();
        return;
    }
    ANativeWindow_release(nw);
    session.SurfaceChanged();
}
