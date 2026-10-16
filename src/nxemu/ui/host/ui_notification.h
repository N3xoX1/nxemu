#pragma once

#include <nxemu-core/notification.h>

#include <mutex>

struct SDL_Window;

class UiNotification final : public INotification
{
public:
    UiNotification();

    // INotification
    void DisplayError(const char* message, const char* title) const override;
    NotificationResponse Query(const char* message, const char* title) const override;
    void BreakPoint(const char* file_name, uint32_t line_number) override;

    void BindWindow(SDL_Window* window);
    void Close();

private:
    mutable std::mutex mutex_;
    SDL_Window* window_ = nullptr;
};
