#pragma once
#include <common/path.h>
#include <nxemu/user_interface/hotkey_map.h>
#include <string>
#include <vector>

typedef std::vector<std::string> Stringlist;

struct UISettings
{
    Stringlist recentFiles;
    Stringlist gameDirectories;
    Path languageDir;
    std::string languageDirValue;
    std::string languageBase;
    std::string languageCurrent;
    bool sciterConsole;
    int32_t gameBrowserMyGameIconSize;
    bool performVulkanCheck;
    bool hasBrokenVulkan;
    bool enableAllControllers;
    bool startGamesInFullscreen;
    bool confirmBeforeStopping;
    HotkeyMap hotkeys;
};

extern UISettings uiSettings;

void SetupUISetting(void);
void SaveUISetting(void);
