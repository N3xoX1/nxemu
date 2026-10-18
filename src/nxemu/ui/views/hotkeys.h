#pragma once
#include "shortcut.h"
#include <cstdint>
#include <nxemu/ui/i18n/translation.h>
#include <string>

enum class HotkeyId
{
    LoadFile,
    Settings,
    Controllers,
    Exit,
    Fullscreen,
    ExitFullscreen,
    HideUi,
    Performance,
    PauseOrContinue,
    Stop,
    DockedMode,
    SpeedLimit,
};

namespace Hotkeys
{
struct Info;
struct GamepadShortcut
{
    std::string guid;
    uint64_t buttons = 0;
    bool operator==(const GamepadShortcut &) const = default;
    std::string Label() const;
};
GamepadShortcut TrackGamepad(const SDL_Event & event);
GamepadShortcut GetGamepad(HotkeyId id);
const Info * GamepadHolder(const GamepadShortcut & shortcut, HotkeyId except);
void SetGamepad(HotkeyId id, const GamepadShortcut & shortcut);

struct Info
{
    HotkeyId id;
    const char * name;
    const char * label;
    bool inGame;
};
inline constexpr Info kAll[] = {
    {HotkeyId::LoadFile, "LoadFile", TrMark("Load a file"), false},
    {HotkeyId::Settings, "Settings", TrMark("Settings"), false},
    {HotkeyId::Controllers, "Controllers", TrMark("Controllers"), false},
    {HotkeyId::Exit, "Exit", TrMark("Exit"), false},
    {HotkeyId::Fullscreen, "Fullscreen", TrMark("Fullscreen"), true},
    {HotkeyId::ExitFullscreen, "ExitFullscreen", TrMark("Leave fullscreen"), true},
    {HotkeyId::HideUi, "HideUi", TrMark("Hide or show the interface"), true},
    {HotkeyId::Performance, "Performance", TrMark("Show performance"), true},
    {HotkeyId::PauseOrContinue, "PauseContinue", TrMark("Pause or continue"), true},
    {HotkeyId::Stop, "StopEmulation", TrMark("Stop the game"), true},
    {HotkeyId::DockedMode, "ToggleDockedMode", TrMark("Docked or handheld"), true},
    {HotkeyId::SpeedLimit, "ToggleSpeedLimit", TrMark("Limit speed"), true},
};

Shortcut Default(HotkeyId id);
Shortcut Get(HotkeyId id);
bool Usable(const Shortcut & shortcut);
const Info * Holder(const Shortcut & shortcut, HotkeyId except);
void Set(HotkeyId id, const Shortcut & shortcut);
void RestoreDefaults();
unsigned Revision();
} // namespace Hotkeys
