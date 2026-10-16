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
        if (window && std::this_thread::get_id() != ui_thread_)
        {
            messages_.push_back(std::move(message));
            return;
        }
    }
    if (window)
        Show(message);
    else if (message.answer)
        message.answer->set_value(NotificationResponse::No);
}

void UiNotification::Show(const Message& message) const
{
    if (!message.question)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, message.title.c_str(),
                                 message.text.c_str(), window_);
        return;
    }
    const std::string yes = Tr("Yes"), no = Tr("No");
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes.c_str()},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, no.c_str()},
    };
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION, window_,
                                  message.title.c_str(), message.text.c_str(),
                                  2, buttons, nullptr};
    int button = 0;
    const bool accepted = SDL_ShowMessageBox(&box, &button) == 0 && button == 1;
    message.answer->set_value(accepted ? NotificationResponse::Yes : NotificationResponse::No);
}
