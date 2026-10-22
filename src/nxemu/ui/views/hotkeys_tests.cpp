#define SDL_MAIN_HANDLED
#include "hotkeys.h"
#include <nxemu/ui/library/ui_preferences.h>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

// Isolate preferences and text services; exercise the production shortcut
// storage and SDL event tracking without opening the emulator's settings.
namespace
{
std::unordered_map<std::string, std::string> preferences;
void Check(bool condition, const char * message)
{
    if (!condition) throw std::runtime_error(message);
}
}
std::string UiPreferences::GetString(const char * key, const std::string & fallback)
{
    const auto found = preferences.find(key);
    return found == preferences.end() ? fallback : found->second;
}
void UiPreferences::SetString(const char * key, const std::string & value) { preferences[key] = value; }
std::string Tr(const char * text) { return text; }
std::string Tr(const char * text, std::initializer_list<std::string> values)
{
    std::string result = text;
    size_t index = 1;
    for (const auto & value : values)
    {
        const std::string token = "%" + std::to_string(index++);
        const auto pos = result.find(token);
        if (pos != std::string::npos) result.replace(pos, token.size(), value);
    }
    return result;
}

int main()
{
    try
    {
        SDL_SetMainReady();
        Check(SDL_Init(SDL_INIT_JOYSTICK) == 0, SDL_GetError());
        const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 0, 8, 0);
        Check(index >= 0, "Virtual joystick unavailable");
        auto * joystick = SDL_JoystickOpen(index);
        Check(joystick != nullptr, "Cannot open virtual joystick");
        const auto device = SDL_JoystickInstanceID(joystick);
        const auto button = [device](Uint32 type, Uint8 number) {
            SDL_Event event{};
            event.type = type;
            event.jbutton.which = device;
            event.jbutton.button = number;
            return Hotkeys::TrackGamepad(event);
        };
        auto single = button(SDL_JOYBUTTONDOWN, 0);
        Check(single.buttons == 1 && single.guid.size() == 32, "First button and device identity");
        auto chord = button(SDL_JOYBUTTONDOWN, 4);
        Check(chord.buttons == 17, "Two-button combination");
        Check(button(SDL_JOYBUTTONUP, 0) == chord, "Release captures the complete combination");
        Check(button(SDL_JOYBUTTONUP, 4).buttons == 16, "Release clears only its own button");
        Check(button(SDL_JOYBUTTONDOWN, 2).buttons == 4, "No held buttons after release");
        SDL_Event removed{};
        removed.type = SDL_JOYDEVICEREMOVED;
        removed.jdevice.which = device;
        Hotkeys::TrackGamepad(removed);
        Check(button(SDL_JOYBUTTONDOWN, 1).buttons == 2, "Removal clears held state");
        // Load one saved binding and ignore malformed persisted data.
        preferences["Shortcuts\\Gamepad\\PauseContinue"] = chord.guid + ":11";
        preferences["Shortcuts\\Gamepad\\StopEmulation"] = chord.guid + ":invalid";
        preferences["Shortcuts\\HideUi"] = "None";
        Check(Hotkeys::Get(HotkeyId::HideUi).key == SDLK_UNKNOWN, "Cleared keyboard shortcut stays cleared on load");
        Check(!Hotkeys::Get(HotkeyId::HideUi).Matches(SDL_Keysym{.sym = SDLK_F4}), "Cleared shortcut cannot trigger");
        Check(Hotkeys::GetGamepad(HotkeyId::PauseOrContinue) == chord, "Saved chord round trip");
        Check(Hotkeys::GetGamepad(HotkeyId::Stop).buttons == 0, "Malformed binding ignored");
        Check(Hotkeys::GamepadHolder(chord, HotkeyId::Stop)->id == HotkeyId::PauseOrContinue, "Duplicate detection");
        const auto revision = Hotkeys::Revision();
        Hotkeys::SetGamepad(HotkeyId::Stop, single);
        Check(Hotkeys::Revision() == revision + 1, "Binding revision");
        Check(preferences["Shortcuts\\Gamepad\\StopEmulation"] == single.guid + ":1", "Persist GUID and button mask");
        Hotkeys::SetGamepad(HotkeyId::Settings, chord);
        Check(Hotkeys::GetGamepad(HotkeyId::Settings).buttons == 0, "Application actions cannot bind gamepads");
        Hotkeys::Set(HotkeyId::Stop, {});
        Check(preferences["Shortcuts\\StopEmulation"] == "None", "Cleared keyboard shortcut has an explicit persisted value");
        Check(Hotkeys::Holder({}, HotkeyId::PauseOrContinue) == nullptr, "Unassigned keys do not conflict");
        Hotkeys::RestoreDefaults();
        Check(Hotkeys::Get(HotkeyId::Stop) == Hotkeys::Default(HotkeyId::Stop) &&
              Hotkeys::Get(HotkeyId::HideUi) == Hotkeys::Default(HotkeyId::HideUi), "Defaults restore cleared keyboard shortcuts");
        Check(Hotkeys::GetGamepad(HotkeyId::Stop).buttons == 0 && preferences["Shortcuts\\Gamepad\\StopEmulation"].empty(), "Defaults clear gamepad bindings");
        SDL_JoystickClose(joystick);
        SDL_JoystickDetachVirtual(index);
        SDL_Quit();
        std::cout << "Keyboard clearing, gamepad chords, releases, disconnects, persistence, conflicts and defaults passed\n";
        return 0;
    }
    catch (const std::exception & error)
    {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return 1;
    }
}
