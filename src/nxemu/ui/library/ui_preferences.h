#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

// Frontend preferences stored in the "UI" section of the settings file. The
// keys are the ones existing settings files already use.
namespace UiPreferences
{
constexpr const char * RecentFiles = "RecentFiles";
constexpr const char * GameDirectories = "GameDirectories";
constexpr const char * GameIconSize = "GameBrowser\\MyGameIconSize";
constexpr const char * GameListView = "GameBrowser\\ListView";
constexpr const char * GameSort = "GameBrowser\\Sort";
constexpr const char * ShowCardInformation = "GameBrowser\\ShowCardInformation";
constexpr const char * ShowPlayTime = "GameBrowser\\ShowPlayTime";
constexpr const char * ShowCardTitle = "GameBrowser\\ShowCardTitle";
constexpr const char * ShowGameDetails = "GameBrowser\\ShowDetails";
constexpr const char * StartGamesInFullscreen = "StartGamesInFullscreen";
constexpr const char * StartGamesWithUiHidden = "StartGamesWithUiHidden";
constexpr const char * ShowPerformance = "ShowPerformance";
constexpr const char * ShowEmulationControls = "ShowEmulationControls";
constexpr const char * ConfirmBeforeStopping = "ConfirmBeforeStopping";
constexpr const char * ConsoleLanguageSet = "ConsoleLanguageSet";
constexpr const char * PerformVulkanCheck = "PerformVulkanCheck";
constexpr const char * EnableDiscordPresence = "EnableDiscordPresence";
constexpr const char * InterfaceLanguage = "InterfaceLanguage";
constexpr const char * InterfaceTheme = "InterfaceTheme"; // system, light or dark

std::string InterfaceLanguageChoice(); // Empty means follow the global console language.
std::string InterfaceLanguageCode();
// Explicit choices in the status bar override global and per-game settings.
void ApplyStatusOverrides();
void SetStatusInt(const char * setting, int32_t value);
void SetStatusBool(const char * setting, bool value);
void ResetStatusOverride(const char * setting);
// Whether the status bar chose one of these settings, or any at all.
bool HasStatusOverride(std::initializer_list<const char *> settings);
bool HasStatusOverride();
std::string GetString(const char * key, const std::string & defaultValue);
void SetString(const char * key, const std::string & value);
std::vector<std::string> GetStringList(const char * key);
void SetStringList(const char * key, const std::vector<std::string> & values);
bool GetBool(const char * key, bool defaultValue);
void SetBool(const char * key, bool value);
int32_t GetInt(const char * key, int32_t defaultValue);
void SetInt(const char * key, int32_t value);
} // namespace UiPreferences
