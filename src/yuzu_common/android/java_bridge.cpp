// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/android/java_bridge.h"

#include <string_view>

#include "yuzu_common/logging/log.h"
#include "yuzu_common/string_util.h"

static JavaVM * s_java_vm = nullptr;
static jobject s_native_library_global = nullptr;
static constexpr jint k_jni_version = JNI_VERSION_1_6;

namespace
{

bool g_jni_caches_initialized = false;

jclass g_double_class = nullptr;
jmethodID g_double_ctor = nullptr;
jmethodID g_double_double_value = nullptr;
jclass g_integer_class = nullptr;
jmethodID g_integer_ctor = nullptr;
jmethodID g_integer_int_value = nullptr;
jclass g_boolean_class = nullptr;
jmethodID g_boolean_ctor = nullptr;
jmethodID g_boolean_boolean_value = nullptr;

void ClearPendingJniException(JNIEnv * env)
{
    if (env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

void InitJavaBoxCache(JNIEnv * env)
{
    jclass local_double = env->FindClass("java/lang/Double");
    jclass local_integer = env->FindClass("java/lang/Integer");
    jclass local_boolean = env->FindClass("java/lang/Boolean");
    if (!local_double || !local_integer || !local_boolean)
    {
        LOG_ERROR(Common, "Java bridge failed to resolve boxed primitive classes");
        ClearPendingJniException(env);
        if (local_double)
        {
            env->DeleteLocalRef(local_double);
        }
        if (local_integer)
        {
            env->DeleteLocalRef(local_integer);
        }
        if (local_boolean)
        {
            env->DeleteLocalRef(local_boolean);
        }
        return;
    }

    g_double_class = static_cast<jclass>(env->NewGlobalRef(local_double));
    g_double_ctor = env->GetMethodID(local_double, "<init>", "(D)V");
    g_double_double_value = env->GetMethodID(local_double, "doubleValue", "()D");
    g_integer_class = static_cast<jclass>(env->NewGlobalRef(local_integer));
    g_integer_ctor = env->GetMethodID(local_integer, "<init>", "(I)V");
    g_integer_int_value = env->GetMethodID(local_integer, "intValue", "()I");
    g_boolean_class = static_cast<jclass>(env->NewGlobalRef(local_boolean));
    g_boolean_ctor = env->GetMethodID(local_boolean, "<init>", "(Z)V");
    g_boolean_boolean_value = env->GetMethodID(local_boolean, "booleanValue", "()Z");
    env->DeleteLocalRef(local_double);
    env->DeleteLocalRef(local_integer);
    env->DeleteLocalRef(local_boolean);

    if (!g_double_class || !g_integer_class || !g_boolean_class || !g_double_ctor || !g_integer_ctor ||
        !g_boolean_ctor || !g_double_double_value || !g_integer_int_value ||
        !g_boolean_boolean_value)
    {
        LOG_ERROR(Common, "Java bridge failed to resolve boxed primitive members");
        ClearPendingJniException(env);
    }
}

void InitJniCaches(JNIEnv * env)
{
    InitJavaBoxCache(env);
    if (g_double_class != nullptr)
    {
        g_jni_caches_initialized = true;
    }
}

} // namespace

JavaVM * GetJavaVM()
{
    return s_java_vm;
}

void SetNativeLibraryGlobalRef(jobject clazz)
{
    s_native_library_global = clazz;
}

jobject GetNativeLibraryGlobalRef()
{
    return s_native_library_global;
}

void ClearNativeLibraryGlobalRef(JNIEnv* env)
{
    if (env != nullptr && s_native_library_global != nullptr)
    {
        env->DeleteGlobalRef(s_native_library_global);
    }
    s_native_library_global = nullptr;
}

JNIEnv * GetEnvForThread()
{
    if (s_java_vm == nullptr)
    {
        return nullptr;
    }
    thread_local static struct OwnedEnv
    {
        OwnedEnv()
        {
            status = s_java_vm->GetEnv(reinterpret_cast<void **>(&env), k_jni_version);
            if (status == JNI_EDETACHED)
            {
                s_java_vm->AttachCurrentThread(&env, nullptr);
            }
        }

        ~OwnedEnv()
        {
            if (status == JNI_EDETACHED)
            {
                s_java_vm->DetachCurrentThread();
            }
        }

        int status{};
        JNIEnv * env = nullptr;
    } owned;
    return owned.env;
}

void SetJavaVM(JavaVM * vm)
{
    if (vm == nullptr)
    {
        return;
    }
    if (s_java_vm == vm && g_jni_caches_initialized)
    {
        return;
    }

    s_java_vm = vm;

    JNIEnv * const env = GetEnvForThread();
    if (env == nullptr)
    {
        LOG_ERROR(Common, "Java bridge SetJavaVM: could not attach JNI environment");
        return;
    }

    InitJniCaches(env);
}

std::string GetJString(JNIEnv * env, jstring jstr)
{
    if (!jstr)
    {
        return {};
    }

    const jchar * jchars = env->GetStringChars(jstr, nullptr);
    const jsize length = env->GetStringLength(jstr);
    const std::u16string_view string_view(reinterpret_cast<const char16_t *>(jchars),
                                          static_cast<u32>(length));
    const std::string converted_string = Common::UTF16ToUTF8(string_view);
    env->ReleaseStringChars(jstr, jchars);

    return converted_string;
}

jstring ToJString(JNIEnv * env, std::string_view str)
{
    const std::u16string converted_string = Common::UTF8ToUTF16(str);
    return env->NewString(reinterpret_cast<const jchar *>(converted_string.data()),
                          static_cast<jint>(converted_string.size()));
}

jstring ToJString(JNIEnv * env, std::u16string_view str)
{
    return ToJString(env, Common::UTF16ToUTF8(str));
}

double GetJDouble(JNIEnv * env, jobject jdouble)
{
    if (!g_double_double_value)
    {
        return 0.0;
    }
    return env->CallDoubleMethod(jdouble, g_double_double_value);
}

jobject ToJDouble(JNIEnv * env, double value)
{
    if (!g_double_class || !g_double_ctor)
    {
        return nullptr;
    }
    return env->NewObject(g_double_class, g_double_ctor, value);
}

s32 GetJInteger(JNIEnv * env, jobject jinteger)
{
    if (!g_integer_int_value)
    {
        return 0;
    }
    return env->CallIntMethod(jinteger, g_integer_int_value);
}

jobject ToJInteger(JNIEnv * env, s32 value)
{
    if (!g_integer_class || !g_integer_ctor)
    {
        return nullptr;
    }
    return env->NewObject(g_integer_class, g_integer_ctor, value);
}

bool GetJBoolean(JNIEnv * env, jobject jboolean)
{
    if (!g_boolean_boolean_value)
    {
        return false;
    }
    return env->CallBooleanMethod(jboolean, g_boolean_boolean_value) != JNI_FALSE;
}

jobject ToJBoolean(JNIEnv * env, bool value)
{
    if (!g_boolean_class || !g_boolean_ctor)
    {
        return nullptr;
    }
    return env->NewObject(g_boolean_class, g_boolean_ctor,
                          static_cast<jboolean>(value ? JNI_TRUE : JNI_FALSE));
}

jmethodID GetIntegerIntValue()
{
    return g_integer_int_value;
}
