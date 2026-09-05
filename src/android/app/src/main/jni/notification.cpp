#include "notification.h"
#include "settings/ui_settings.h"
#include <android/log.h>

namespace
{
    constexpr const char * kLogTag = "NxEmu";
}

AndroidNotification::AndroidNotification()
{
}

void AndroidNotification::DisplayError(const char * message, const char * title) const
{
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s: %s", title != nullptr ? title : "",
                        message != nullptr ? message : "");
}

NotificationResponse AndroidNotification::Query(const char * message, const char * title) const
{
    __android_log_print(ANDROID_LOG_WARN, kLogTag, "Query %s: %s", title != nullptr ? title : "",
                        message != nullptr ? message : "");
    return NotificationResponse::No;
}

void AndroidNotification::BreakPoint(const char * fileName, uint32_t lineNumber)
{
    __android_log_print(ANDROID_LOG_ERROR, kLogTag, "BreakPoint %s:%u",
                        fileName != nullptr ? fileName : "", lineNumber);
}

void AndroidNotification::AppInitDone()
{
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "AppInit done");
    SetupUISetting();
}

AndroidNotification & AndroidNotification::GetInstance()
{
    static AndroidNotification instance;
    return instance;
}
