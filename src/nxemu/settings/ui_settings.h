#pragma once
#include <common/path.h>
#include <map>
#include <nxemu/user_interface/hotkey_map.h>
#include <string>
#include <vector>

typedef std::vector<std::string> Stringlist;
typedef std::map<std::string, std::string> StringMap;

struct UISettings
{
    // Both interfaces
    Stringlist recentFiles;
    Stringlist gameDirectories;
    bool performVulkanCheck;
    bool startGamesInFullscreen;
    bool startGamesWithUiHidden;
    bool confirmBeforeStopping;
    bool enableDiscordPresence;

    // New interface
    int32_t gameIconSize;
    int32_t gameSort;
    bool gameListView;
    bool showCardInformation;
    bool showCardTitle;
    bool showPlayTime;
    bool showEmulationControls;
    StringMap shortcuts;

    // Sciter only: remove with Sciter
    Path languageDir;
    std::string languageDirValue;
    std::string languageBase;
    std::string languageCurrent;
    bool sciterConsole;
    bool useRmlUI;
    int32_t gameBrowserMyGameIconSize;
    bool hasBrokenVulkan;
    bool enableAllControllers;
    bool hideMouseOnInactivity;
    HotkeyMap hotkeys;
};

extern UISettings uiSettings;

void SetupUISetting(void);
void SaveUISetting(void);
void SetUISettingStringList(const char * identifier, const Stringlist & value);
