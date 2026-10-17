#pragma once

#include <nxemu-core/notification.h>

#include <deque>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct SDL_Window;

class UiNotification final : public INotification
{
public:
    UiNotification();

    void DisplayError(const char* message, const char* title) const override;
    NotificationResponse Query(const char* message, const char* title) const override;
    void BreakPoint(const char* file_name, uint32_t line_number) override;

    void BindWindow(SDL_Window* window);
    using Presenter = std::function<NotificationResponse(bool, const std::string &, const std::string &)>;
    void SetPresenter(Presenter presenter);
    void Pump();
    void Close();

private:
    struct Message
    {
        bool question;
        std::string title;
        std::string text;
        std::shared_ptr<std::promise<NotificationResponse>> answer;
    };

    void QueueOrShow(Message message) const;
    void Show(const Message& message) const;

    const std::thread::id ui_thread_;
    mutable std::mutex mutex_;
    mutable std::deque<Message> messages_;
    SDL_Window* window_ = nullptr;
    Presenter presenter_;
};
