#include "user_profiles.h"
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>

int UserProfiles::Active(size_t count)
{
    const int active = SettingsStore::GetInstance().GetInt(NXOsSetting::CurrentUser);
    return active >= 0 && static_cast<size_t>(active) < count ? active : 0;
}

void UserProfiles::SetActive(int index)
{
    SettingsStore::GetInstance().SetInt(NXOsSetting::CurrentUser, index);
}
