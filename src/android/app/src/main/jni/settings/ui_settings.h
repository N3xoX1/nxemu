#pragma once
#include <string>
#include <vector>

typedef std::vector<std::string> Stringlist;

struct UISettings
{
    Stringlist gameDirectories;
};

extern UISettings uiSettings;

void SetupUISetting();
void SaveUISetting();