#include "logging.h"
#include "settings/core_settings.h"
#include "settings/identifiers.h"
#include "settings/settings.h"
#include <string.h>
#include <yuzu_common/logging/backend.h>

#ifdef WIN32
extern "C" __declspec(dllimport) int __stdcall AllocConsole(void);
extern "C" __declspec(dllimport) void * __stdcall GetConsoleWindow(void);
extern "C" __declspec(dllimport) int __stdcall ShowWindow(void*, int);
#endif

void UpdateLogConsole()
{
    Common::Log::SetColorConsoleBackendEnabled(coreSettings.ShowLogConsole);
#ifdef WIN32
    void * consoleWindow = GetConsoleWindow();
    if (coreSettings.ShowLogConsole)
    {
        if (consoleWindow != nullptr)
        {
            ShowWindow(GetConsoleWindow(), 5);
        }
        else if (AllocConsole())
        {
            FILE* fp;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
            freopen_s(&fp, "CONIN$", "r", stdin);
        }
    }
    else if(consoleWindow != nullptr)
    {
        ShowWindow(consoleWindow, 0);
    }
#endif
}

void LoggingSettingChanged(const char * setting, void * /*userData*/)
{
    if (strcmp(setting, NXCoreSetting::ShowLogConsole) == 0)
    {
        UpdateLogConsole();
    }
    if (strcmp(setting, NXCoreSetting::LogFilter) == 0)
    {
        SettingsStore& settingsStore = SettingsStore::GetInstance();
        Common::Log::ResetFilter(settingsStore.GetString(NXCoreSetting::LogFilter));
    }
}

void LoggingSetup(void)
{
    SettingsStore & settingsStore = SettingsStore::GetInstance();
    settingsStore.RegisterCallback(NXCoreSetting::LogFilter, LoggingSettingChanged, nullptr);
    settingsStore.RegisterCallback(NXCoreSetting::ShowLogConsole, LoggingSettingChanged, nullptr);

    Common::Log::Initialize(nullptr, settingsStore.GetString(NXCoreSetting::LogFilter));
    Common::Log::Start();
    UpdateLogConsole();
}

void LoggingShutdown(void)
{
    Common::Log::Stop();
}