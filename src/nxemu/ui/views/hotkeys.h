#pragma once
#include "shortcut.h"
#include <nxemu/ui/i18n/translation.h>

// What a key can start, in the order the settings list it.
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

// The key of each, which the user may change: the settings keep the ones
// that are not the usual ones.
namespace Hotkeys
{
struct Info
{
    HotkeyId id;
    const char * name; // in the settings file
    const char * label;
    bool inGame; // acts on the game that runs
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
    {HotkeyId::DockedMode, "ToggleDockedMode", TrMark("TV or handheld"), true},
    {HotkeyId::SpeedLimit, "ToggleSpeedLimit", TrMark("Limit speed"), true},
};

Shortcut Default(HotkeyId id);
Shortcut Get(HotkeyId id);
// A key can be a shortcut when it is not one that is typed: a function key,
// Escape, or any key with Control, Command or Alt.
bool Usable(const Shortcut & shortcut);
// The other action that has the shortcut, or nullptr.
const Info * Holder(const Shortcut & shortcut, HotkeyId except);
void Set(HotkeyId id, const Shortcut & shortcut);
void RestoreDefaults();
// Changes each time a shortcut does, for those that show them.
unsigned Revision();
} // namespace Hotkeys
