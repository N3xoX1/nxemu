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
    ThemeMode themeMode;
};

extern UISettings uiSettings;

void SetupUISetting();
void SaveUISetting();