#pragma once

#include <common/json.h>
#include <map>
#include <string>
#include <widgets/menubar.h>

namespace Hotkey
{
constexpr const char LoadFile[] = "LoadFile";
constexpr const char Exit[] = "Exit";
constexpr const char Fullscreen[] = "Fullscreen";
constexpr const char ExitFullscreen[] = "ExitFullscreen";
constexpr const char HideUi[] = "HideUi";
constexpr const char PauseContinue[] = "PauseContinue";
constexpr const char ToggleDockedMode[] = "ToggleDockedMode";
constexpr const char ToggleSpeedLimit[] = "ToggleSpeedLimit";
constexpr const char StopEmulation[] = "StopEmulation";
constexpr const char Configure[] = "Configure";
constexpr const char Controllers[] = "Controllers";
}

using HotkeyMap = std::map<std::string, MenuBarAccelerator>;

MenuBarAccelerator MenuBarAcceleratorFromKeyEvent(uint32_t keyCode, uint32_t keyboardState);

JsonValue HotkeyMapToJsonObject(const HotkeyMap & m);
JsonValue HotkeyMapToJsonObjectDiff(const HotkeyMap & current, const HotkeyMap & defaults);
std::string SerializeUIHotkeysMap(const HotkeyMap & m);
bool DeserializeHotkeyMapFromJsonObject(const JsonValue & root, HotkeyMap & inOut);
bool DeserializeUIHotkeysMap(const std::string & json, HotkeyMap & out);
