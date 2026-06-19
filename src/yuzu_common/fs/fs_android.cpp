// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/android/java_bridge.h"
#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/fs/fs_android.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/string_util.h"

#include <filesystem>

namespace Common::FS::Android {

void RegisterCallbacks(JNIEnv* env, jclass clazz) {
    env->GetJavaVM(&g_jvm);
    if (native_library != nullptr) {
        env->DeleteGlobalRef(native_library);
    }
    native_library = static_cast<jclass>(env->NewGlobalRef(clazz));

    s_get_parent_directory = env->GetStaticMethodID(native_library, "getParentDirectory", "(Ljava/lang/String;)Ljava/lang/String;");
    s_get_filename = env->GetStaticMethodID(native_library, "getFilename", "(Ljava/lang/String;)Ljava/lang/String;");
    s_get_size = env->GetStaticMethodID(native_library, "getSize", "(Ljava/lang/String;)J");
    s_is_directory = env->GetStaticMethodID(native_library, "isDirectory", "(Ljava/lang/String;)Z");
    s_file_exists = env->GetStaticMethodID(native_library, "exists", "(Ljava/lang/String;)Z");
    s_open_content_uri = env->GetStaticMethodID(native_library, "openContentUri", "(Ljava/lang/String;Ljava/lang/String;)I");
}

void UnRegisterCallbacks(JNIEnv* env) {
    s_get_parent_directory = nullptr;
    s_get_filename = nullptr;

    s_get_size = nullptr;
    s_is_directory = nullptr;
    s_file_exists = nullptr;

    s_open_content_uri = nullptr;

    if (env != nullptr && native_library != nullptr) {
        env->DeleteGlobalRef(native_library);
        native_library = nullptr;
    }
}

bool IsContentUri(const std::string& path) {
    constexpr std::string_view prefix = "content://";
    if (path.size() < prefix.size()) [[unlikely]] {
        return false;
    }

    return path.find(prefix) == 0;
}

s32 OpenContentUri(const std::string& filepath, OpenMode openmode) {
    if (s_open_content_uri == nullptr)
        return -1;

    const char* mode = "";
    switch (openmode) {
    case OpenMode::Read:
        mode = "r";
        break;
    default:
        UNIMPLEMENTED();
        return -1;
    }
    auto env = GetEnvForThread();
    jstring j_filepath = ToJString(env, filepath);
    jstring j_mode = ToJString(env, mode);
    return env->CallStaticIntMethod(native_library, s_open_content_uri, j_filepath, j_mode);
}

u64 GetSize(const std::string& filepath) {
    if (s_get_size == nullptr) {
        return 0;
    }
    auto env = GetEnvForThread();
    return static_cast<u64>(env->CallStaticLongMethod(
        native_library, s_get_size,
        ToJString(GetEnvForThread(), filepath)));
}

bool IsDirectory(const std::string& filepath) {
    if (s_is_directory == nullptr) {
        return 0;
    }
    auto env = GetEnvForThread();
    return env->CallStaticBooleanMethod(
        native_library, s_is_directory,
        ToJString(GetEnvForThread(), filepath));
}

bool Exists(const std::string& filepath) {
    if (s_file_exists == nullptr) {
        return 0;
    }
    auto env = GetEnvForThread();
    return env->CallStaticBooleanMethod(
        native_library, s_file_exists,
        ToJString(GetEnvForThread(), filepath));
}

std::string GetParentDirectory(const std::string& filepath) {
    if (s_get_parent_directory == nullptr) {
        return std::filesystem::path(filepath).parent_path().string();
    }
    auto env = GetEnvForThread();
    jstring j_return = static_cast<jstring>(env->CallStaticObjectMethod(
        native_library, s_get_parent_directory, ToJString(env, filepath)));
    if (!j_return) {
        return {};
    }
    return GetJString(env, j_return);
}

std::string GetFilename(const std::string& filepath) {
    if (s_get_filename == nullptr) {
        return std::filesystem::path(filepath).filename().string();
    }
    auto env = GetEnvForThread();
    jstring j_return = static_cast<jstring>(env->CallStaticObjectMethod(
        native_library, s_get_filename, ToJString(env, filepath)));
    if (!j_return) {
        return {};
    }
    return GetJString(env, j_return);
}

} // namespace Common::FS::Android
