#include "ui_host.h"

#include <cstdlib>
#include <yuzu_common/android/java_bridge.h>

UiHost::UiHost(JNIEnv * env, jobject host) :
    m_host(env->NewGlobalRef(host))
{
    jclass cls = env->GetObjectClass(host);
    m_request = env->GetMethodID(cls, "request", "(Ljava/lang/String;[Ljava/lang/String;)Ljava/lang/String;");
    env->DeleteLocalRef(cls);
}

UiHost::~UiHost()
{
    if (JNIEnv * env = GetEnvForThread(); env != nullptr && m_host != nullptr)
    {
        env->DeleteGlobalRef(m_host);
    }
}

std::string UiHost::Request(const char * name, std::initializer_list<std::string> args) const
{
    JNIEnv * env = GetEnvForThread();
    if (env == nullptr || m_host == nullptr || m_request == nullptr)
    {
        return {};
    }
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray jargs = env->NewObjectArray(static_cast<jsize>(args.size()), stringClass, nullptr);
    jsize index = 0;
    for (const std::string & arg : args)
    {
        jstring value = ToJString(env, arg);
        env->SetObjectArrayElement(jargs, index++, value);
        env->DeleteLocalRef(value);
    }
    jstring jname = env->NewStringUTF(name);
    auto result = static_cast<jstring>(env->CallObjectMethod(m_host, m_request, jname, jargs));
    if (env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
        result = nullptr;
    }
    std::string value = result != nullptr ? GetJString(env, result) : std::string();
    if (result != nullptr)
    {
        env->DeleteLocalRef(result);
    }
    env->DeleteLocalRef(jname);
    env->DeleteLocalRef(jargs);
    env->DeleteLocalRef(stringClass);
    return value;
}

bool UiHost::RequestBool(const char * name, std::initializer_list<std::string> args) const
{
    return Request(name, args) == "true";
}

int UiHost::RequestInt(const char * name, std::initializer_list<std::string> args) const
{
    return std::atoi(Request(name, args).c_str());
}
