#pragma once

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

using TitleIdStringListMap = std::map<uint64_t, std::vector<std::string>>;

struct LoaderSettings
{
    bool checkForUpdatedFirmware;

    // Add-Ons
    TitleIdStringListMap disabled_addons;
};

extern LoaderSettings loaderSettings;

void SetupLoaderSetting(void);
void CleanupLoaderSetting(void);
void SaveLoaderSettings(void);

void SetLoaderDisabledAddons(uint64_t program_id, std::vector<std::string> names);
