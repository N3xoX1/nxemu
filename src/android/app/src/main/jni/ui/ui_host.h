#pragma once

#include <initializer_list>
#include <jni.h>
#include <string>

class UiHost
{
public:
    UiHost(JNIEnv * env, jobject host);
    ~UiHost();

    std::string Request(const char * name, std::initializer_list<std::string> args = {}) const;
    bool RequestBool(const char * name, std::initializer_list<std::string> args = {}) const;
    int RequestInt(const char * name, std::initializer_list<std::string> args = {}) const;

private:
    UiHost(const UiHost &) = delete;
    UiHost & operator=(const UiHost &) = delete;

    jobject m_host = nullptr;
    jmethodID m_request = nullptr;
};
