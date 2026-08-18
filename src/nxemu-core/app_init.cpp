#include "app_init.h"
#include "logging.h"
#include "notification.h"
#include "settings/core_settings.h"
#include "settings/identifiers.h"
#include "settings/settings.h"
#include <common/path.h>
#include <string>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/fs/path_util.h>

bool AppInit(INotification * notification, const char * baseDirectory, const char * appDirectory)
{
    if (baseDirectory == nullptr || baseDirectory[0] == '\0' ||
        appDirectory == nullptr || appDirectory[0] == '\0')
    {
        return false;
    }

    const Path app_base(baseDirectory);
    if (!app_base.DirectoryExists())
    {
        return false;
    }

    Path config_path(appDirectory, "NxEmu.config");
    config_path.AppendDirectory("config");

    g_notify = notification;

    SettingsStore & settings_store = SettingsStore::GetInstance();
    if (!settings_store.Initialize(config_path, baseDirectory))
    {
        return false;
    }

    SetupCoreSetting();

    settings_store.SetString(NXCoreSetting::AppBaseDirectory, app_base.GetDriveDirectory().c_str());
    settings_store.SetString(NXCoreSetting::AppDirectory, appDirectory);
    Common::FS::SetAppDirectory(std::string(appDirectory));
    LoggingSetup();

    if (notification)
    {
        notification->AppInitDone();
    }
    return true;
}

void AppCleanup(void)
{
    LoggingShutdown();
    SettingsStore::CleanUp();
}
