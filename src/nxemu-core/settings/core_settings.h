#pragma once
#include "common/path.h"
#include <string>

struct CoreSettings
{
    bool ShowLogConsole;
    std::string LogFilter;
    Path baseDir;
    Path appDir;
    Path configDir;
    Path moduleDir;
    std::string moduleDirValue;
    std::string moduleLoader;
    std::string moduleCpu;
    std::string moduleVideo;
    std::string moduleOs;
    std::string gpuHookLibDir;
    std::string gpuCustomDriverDir;
    std::string gpuCustomDriverName;
    std::string gpuFileRedirectDir;
};

extern CoreSettings coreSettings;

void SetupCoreSetting(void);
void SaveCoreSetting(void);