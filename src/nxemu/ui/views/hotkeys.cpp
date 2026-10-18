#include "hotkeys.h"
#include <array>
#include <iterator>
#include <nxemu/ui/library/ui_preferences.h>

namespace
{
constexpr const char * kSection = "Shortcuts\\";

struct State
{
    std::array<Shortcut, std::size(Hotkeys::kAll)> shortcuts;
    unsigned revision = 0;
};

State & Current()
{
    static State state = [] {
        State read;
        for (const Hotkeys::Info & info : Hotkeys::kAll)
        {
            const Shortcut chosen = Shortcut::Parse(UiPreferences::GetString((std::string(kSection) + info.name).c_str(), ""));
            read.shortcuts[static_cast<size_t>(info.id)] = Hotkeys::Usable(chosen) ? chosen : Hotkeys::Default(info.id);
        }
        return read;
    }();
    return state;
}
} // namespace

Shortcut Hotkeys::Default(HotkeyId id)
{
    switch (id)
    {
    case HotkeyId::LoadFile: return {SDLK_o, true};
    case HotkeyId::Settings: return {SDLK_t, true};
    case HotkeyId::Controllers: return {SDLK_c, true};
    case HotkeyId::Exit: return {SDLK_q, true};
    case HotkeyId::Fullscreen: return CommandKeyboard() ? Shortcut{SDLK_f, true, false, true} : Shortcut{SDLK_F11};
    case HotkeyId::ExitFullscreen: return {SDLK_ESCAPE};
    case HotkeyId::HideUi: return {SDLK_F4};
    case HotkeyId::Performance: return {SDLK_F3};
    case HotkeyId::PauseOrContinue: return {SDLK_F2};
    case HotkeyId::Stop: return {SDLK_F5};
    case HotkeyId::DockedMode: return {SDLK_F10};
    case HotkeyId::SpeedLimit: return {SDLK_u, true};
    }
    return {};
}

Shortcut Hotkeys::Get(HotkeyId id)
{
    return Current().shortcuts[static_cast<size_t>(id)];
}

bool Hotkeys::Usable(const Shortcut & shortcut)
{
    return shortcut.key != SDLK_UNKNOWN &&
           (shortcut.ctrl || shortcut.alt || shortcut.key == SDLK_ESCAPE || (shortcut.key >= SDLK_F1 && shortcut.key <= SDLK_F12));
}

const Hotkeys::Info * Hotkeys::Holder(const Shortcut & shortcut, HotkeyId except)
{
    for (const Info & info : kAll)
    {
        if (info.id != except && Get(info.id) == shortcut)
        {
            return &info;
        }
    }
    return nullptr;
}

void Hotkeys::Set(HotkeyId id, const Shortcut & shortcut)
{
    State & state = Current();
    state.shortcuts[static_cast<size_t>(id)] = shortcut;
    state.revision++;
    UiPreferences::SetString((std::string(kSection) + kAll[static_cast<size_t>(id)].name).c_str(), shortcut == Default(id) ? std::string() : shortcut.Text());
}

void Hotkeys::RestoreDefaults()
{
    for (const Info & info : kAll)
    {
        if (!(Get(info.id) == Default(info.id)))
        {
            Set(info.id, Default(info.id));
        }
    }
}

unsigned Hotkeys::Revision()
{
    return Current().revision;
}
