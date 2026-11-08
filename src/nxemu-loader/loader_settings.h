#pragma once

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

using TitleIdStringListMap = std::map<uint64_t, std::vector<std::string>>;
using StringList = std::vector<std::string>;

struct LoaderSettings
{
    bool checkForUpdatedFirmware;
    int32_t languageIndex;

    // Add-Ons
    TitleIdStringListMap disabled_addons;
    // The bundled mods that are off by default (mod.json "enabled_by_default": false) the user turned on
    TitleIdStringListMap enabled_addons;
    // Where the mods that ship with NXEmu are, relative to the executable; empty for where the package puts them
    std::string bundledModsDirectory;
};

extern LoaderSettings loaderSettings;

void SetupLoaderSetting(void);
void CleanupLoaderSetting(void);
void SaveLoaderSettings(void);

void SetLoaderAddOns(uint64_t program_id, std::vector<std::string> disabled, std::vector<std::string> enabled);
