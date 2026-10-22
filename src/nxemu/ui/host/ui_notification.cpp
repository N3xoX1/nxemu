#include "ui_notification.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdlib>
#include <iostream>
#include <nxemu/ui/i18n/translation.h>

UiNotification::UiNotification() : ui_thread_(std::this_thread::get_id()) {}

void UiNotification::DisplayError(const char* message, const char* title) const
{
    std::cerr << (title ? title : "NxEmu") << ": " << (message ? message : "") << '\n';
    QueueOrShow({false, title ? title : "NxEmu", message ? message : "", {}});
}

NotificationResponse UiNotification::Query(const char* message, const char* title) const
{
    auto answer = std::make_shared<std::promise<NotificationResponse>>();
    auto result = answer->get_future();
    QueueOrShow({true, title ? title : "NxEmu", message ? message : "", answer});
    return result.get();
}

void UiNotification::BreakPoint(const char* file_name, uint32_t line_number)
{
    std::cerr << "NxEmu breakpoint at " << file_name << ':' << line_number << '\n';
    std::abort();
}

void UiNotification::BindWindow(SDL_Window* window)
{
    std::lock_guard lock(mutex_);
    window_ = window;
}

void UiNotification::SetPresenter(Presenter presenter)
{
    std::lock_guard lock(mutex_);
    presenter_ = std::move(presenter);
}

void UiNotification::Pump()
{
    std::deque<Message> pending;
    {
        std::lock_guard lock(mutex_);
        pending.swap(messages_);
    }
    for (const auto& message : pending)
        Show(message);
}

void UiNotification::Close()
{
    std::deque<Message> pending;
    {
        std::lock_guard lock(mutex_);
        window_ = nullptr;
        pending.swap(messages_);
    }
    for (const auto& message : pending)
    {
        if (message.answer)
            message.answer->set_value(NotificationResponse::No);
    }
}

void UiNotification::QueueOrShow(Message message) const
{
    SDL_Window* window = nullptr;
    {
        std::lock_guard lock(mutex_);
        window = window_;
        if (window && (std::this_thread::get_id() != ui_thread_ || (!message.question && presenter_)))
        {
            messages_.push_back(std::move(message));
            return;
        }
    }
    if (window || std::this_thread::get_id() == ui_thread_)
        Show(message);
    else if (message.answer)
        message.answer->set_value(NotificationResponse::No);
}

void UiNotification::Show(const Message& message) const
{
    Presenter presenter;
    {
        std::lock_guard lock(mutex_);
        presenter = presenter_;
    }
    // Startup failures can happen before RmlUi or its renderer is available.
    if (!presenter && !message.question)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, message.title.c_str(), message.text.c_str(), window_);
    }
    const auto response = presenter ? presenter(message.question, message.title, message.text) : NotificationResponse::No;
    if (message.answer) message.answer->set_value(response);
}
