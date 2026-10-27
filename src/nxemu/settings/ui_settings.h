#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

typedef std::vector<std::string> Stringlist;
typedef std::map<std::string, std::string> StringMap;

struct UISettings
{
    Stringlist recentFiles;
    Stringlist gameDirectories;
    Stringlist gameDirectoriesRecursive;
    bool performVulkanCheck;
    bool startGamesInFullscreen;
    bool startGamesWithUiHidden;
    bool confirmBeforeStopping;
    bool enableDiscordPresence;
    std::string interfaceTheme;
    std::string interfaceLanguage;
    bool consoleLanguageSet;
    int32_t gameIconSize;
    int32_t gameSort;
    bool gameListView;
    bool showCardInformation;
    bool showCardTitle;
    bool showPlayTime;
    bool showGameDetails;
    bool showEmulationControls;
    bool showCpuBackend;
    StringMap shortcuts;
    bool showPerformance;
    bool performanceHistory;
    bool performanceRange;
    bool performanceFrameTime;
    bool performanceLongest;
    bool performanceSpeed;
    bool performanceProcessor;
    bool performanceMemory;
    int32_t performanceSize;
    int32_t performanceRate;
    int32_t performanceHistoryLength;
};

extern UISettings uiSettings;

void SetupUISetting(void);
void SaveUISetting(void);
void SetUISettingStringList(const char * identifier, const Stringlist & value);
