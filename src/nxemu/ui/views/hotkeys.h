#pragma once
#include "shortcut.h"
#include <nxemu/ui/i18n/translation.h>

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
    {HotkeyId::DockedMode, "ToggleDockedMode", TrMark("TV or handheld"), true},
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
