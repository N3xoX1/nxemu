#pragma once

struct LoaderSettings
{
    bool checkForUpdatedFirmware;
};

extern LoaderSettings loaderSettings;

void SetupLoaderSetting(void);
void CleanupLoaderSetting(void);
void SaveLoaderSettings(void);
