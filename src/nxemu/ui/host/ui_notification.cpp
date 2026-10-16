#include "ui_notification.h"
#include "settings/ui_settings.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>


UiNotification::UiNotification()
{
}

void UiNotification::DisplayError(const char * /*message*/, const char * /*title*/) const
{
}

NotificationResponse UiNotification::Query(const char * /*message*/, const char * /*title*/) const
{
    return NotificationResponse::No;
}

void UiNotification::BreakPoint(const char * /*file_name*/, uint32_t /*line_number*/)
{
}

void UiNotification::AppInitDone()
{
    SetupUISetting();
}

void UiNotification::BindWindow(SDL_Window* window)
{
    std::lock_guard lock(mutex_);
    window_ = window;
}

void UiNotification::Close()
{
    {
        std::lock_guard lock(mutex_);
        window_ = nullptr;
    }
}

