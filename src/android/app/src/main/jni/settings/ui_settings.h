#pragma once
#include <cstdint>
#include <string>
#include <vector>

typedef std::vector<std::string> Stringlist;

enum class ThemeMode : int32_t
{
    FollowSystem = 0,
    Light = 1,
    Dark = 2,
};

struct UISettings
{
    Stringlist gameDirectories;
    bool showInputOverlay;
    int32_t overlayScale;
    int32_t overlayOpacity;
    std::string overlayControlData;
    ThemeMode themeMode;
    bool lockDrawer;
    int32_t gameListSort;
    int32_t gameListView;
    int32_t gameCardSize;
    bool showCardTitle;
    bool showCardDetails;
    bool showCardPlayTime;
    std::string interfaceLanguage;
    bool consoleLanguageSet;
};

extern UISettings uiSettings;

void SetupUISetting();
void SaveUISetting();