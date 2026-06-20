// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ANDROID

#include <functional>
#include <future>
#include <string>
#include <string_view>
#include <type_traits>

#include <jni.h>
#include "yuzu_common/common_types.h"

void SetJavaVM(JavaVM* vm);
JavaVM* GetJavaVM();
JNIEnv* GetEnvForThread();

void SetNativeLibraryGlobalRef(jobject clazz);
jobject GetNativeLibraryGlobalRef();
void ClearNativeLibraryGlobalRef(JNIEnv* env);

template <typename T = void>
T RunJNIOnFiber(const std::function<T(JNIEnv*)>& work) {
    std::future<T> j_result = std::async(std::launch::async, [&] {
        auto* env = GetEnvForThread();
        return work(env);
    });
    return j_result.get();
}

template <typename T, typename F>
T RunJNIOnFiber(F&& work) {
    using WorkFn = std::function<T(JNIEnv*)>;
    if constexpr (std::is_same_v<std::decay_t<F>, WorkFn>) {
        return RunJNIOnFiber<T>(static_cast<const WorkFn&>(work));
    } else {
        return RunJNIOnFiber<T>(WorkFn(std::forward<F>(work)));
    }
}

std::string GetJString(JNIEnv* env, jstring jstr);
jstring ToJString(JNIEnv* env, std::string_view str);
jstring ToJString(JNIEnv* env, std::u16string_view str);

double GetJDouble(JNIEnv* env, jobject jdouble);
jobject ToJDouble(JNIEnv* env, double value);

s32 GetJInteger(JNIEnv* env, jobject jinteger);
jobject ToJInteger(JNIEnv* env, s32 value);

bool GetJBoolean(JNIEnv* env, jobject jboolean);
jobject ToJBoolean(JNIEnv* env, bool value);

jmethodID GetInputDeviceGetGUID();
jmethodID GetInputDeviceGetPort();
jmethodID GetInputDeviceGetSupportsVibration();
jmethodID GetInputDeviceGetName();
jmethodID GetInputDeviceGetAxes();
jmethodID GetInputDeviceHasKeys();
jmethodID GetInputDeviceVibrate();
jmethodID GetIntegerIntValue();

#endif
