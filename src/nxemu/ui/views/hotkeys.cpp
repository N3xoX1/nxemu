#include "hotkeys.h"
#include <array>
#include <iterator>
#include <unordered_map>
#include <nxemu/ui/library/ui_preferences.h>

namespace
{
constexpr const char * kSection = "Shortcuts\\";

struct State
{
    std::array<Shortcut, std::size(Hotkeys::kAll)> shortcuts;
    std::array<Hotkeys::GamepadShortcut, std::size(Hotkeys::kAll)> gamepads;
    unsigned revision = 0;
};

// Read from the settings the first time it is asked for.
State & Current()
{
    static State state = [] {
        State read;
        for (const Hotkeys::Info & info : Hotkeys::kAll)
        {
            const std::string storedKey = UiPreferences::GetString((std::string(kSection) + info.name).c_str(), "");
            const Shortcut chosen = Shortcut::Parse(storedKey);
            read.shortcuts[static_cast<size_t>(info.id)] = storedKey == "None" ? Shortcut{} :
                Hotkeys::Usable(chosen) ? chosen : Hotkeys::Default(info.id);
            const std::string stored = UiPreferences::GetString((std::string(kSection) + "Gamepad\\" + info.name).c_str(), "");
            if (info.inGame && stored.size() > 33 && stored[32] == ':')
            {
                try
                {
                    size_t used = 0;
                    const auto buttons = std::stoull(stored.substr(33), &used, 16);
                    if (used == stored.size() - 33 && stored.substr(0, 32).find_first_not_of("0123456789abcdef") == std::string::npos)
                        read.gamepads[static_cast<size_t>(info.id)] = {stored.substr(0, 32), buttons};
                }
                catch (const std::exception &) {}
            }
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
    // macOS keeps F11 for itself.
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
    if (shortcut.key == SDLK_UNKNOWN) return nullptr;
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
    // The usual key is not written: it may change with the version.
    UiPreferences::SetString((std::string(kSection) + kAll[static_cast<size_t>(id)].name).c_str(),
                            shortcut.key == SDLK_UNKNOWN ? "None" : shortcut == Default(id) ? std::string() : shortcut.Text());
}

void Hotkeys::RestoreDefaults()
{
    for (const Info & info : kAll)
    {
        if (GetGamepad(info.id).buttons != 0) SetGamepad(info.id, {});
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

std::string Hotkeys::GamepadShortcut::Label() const
{
    if (buttons == 0) return Tr("Not assigned");
    std::string label;
    for (int button = 0; button < 64; ++button)
    {
        if ((buttons & (uint64_t{1} << button)) == 0) continue;
        if (!label.empty()) label += " + ";
        label += Tr("Button %1", {std::to_string(button + 1)});
    }
    return label;
}

Hotkeys::GamepadShortcut Hotkeys::TrackGamepad(const SDL_Event & event)
{
    static std::unordered_map<SDL_JoystickID, uint64_t> held;
    if (event.type == SDL_JOYDEVICEREMOVED)
    {
        held.erase(event.jdevice.which);
        return {};
    }
    if ((event.type != SDL_JOYBUTTONDOWN && event.type != SDL_JOYBUTTONUP) || event.jbutton.button >= 64) return {};
    SDL_Joystick * joystick = SDL_JoystickFromInstanceID(event.jbutton.which);
    if (joystick == nullptr) return {};
    char guid[33];
    SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joystick), guid, sizeof(guid));
    auto & buttons = held[event.jbutton.which];
    const uint64_t bit = uint64_t{1} << event.jbutton.button;
    if (event.type == SDL_JOYBUTTONDOWN) buttons |= bit;
    GamepadShortcut chord{guid, buttons};
    if (event.type == SDL_JOYBUTTONUP) buttons &= ~bit;
    return chord;
}

Hotkeys::GamepadShortcut Hotkeys::GetGamepad(HotkeyId id)
{
    return Current().gamepads[static_cast<size_t>(id)];
}

const Hotkeys::Info * Hotkeys::GamepadHolder(const GamepadShortcut & shortcut, HotkeyId except)
{
    if (shortcut.buttons == 0) return nullptr;
    for (const Info & info : kAll)
        if (info.inGame && info.id != except && GetGamepad(info.id) == shortcut) return &info;
    return nullptr;
}

void Hotkeys::SetGamepad(HotkeyId id, const GamepadShortcut & shortcut)
{
    if (!kAll[static_cast<size_t>(id)].inGame) return;
    Current().gamepads[static_cast<size_t>(id)] = shortcut;
    ++Current().revision;
    char buttons[17];
    SDL_snprintf(buttons, sizeof(buttons), "%llx", static_cast<unsigned long long>(shortcut.buttons));
    UiPreferences::SetString((std::string(kSection) + "Gamepad\\" + kAll[static_cast<size_t>(id)].name).c_str(),
                             shortcut.buttons == 0 ? std::string() : shortcut.guid + ":" + buttons);
}
