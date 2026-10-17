#include "user_profiles.h"
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>

std::vector<UserProfile> UserProfiles::List(IOperatingSystem & os)
{
    std::vector<UserProfile> profiles;
    const uint32_t count = os.GetProfileCount();
    for (uint32_t index = 0; index < count; index++)
    {
        UserProfile profile{};
        if (!os.GetProfile(index, &profile.info))
        {
            continue;
        }
        profile.name = profile.info.username;
        char path[1024] = {};
        if (os.GetProfileImagePath(profile.info.uuid, path, sizeof(path)))
        {
            profile.picture = path;
        }
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

int UserProfiles::Active(size_t count)
{
    const int active = SettingsStore::GetInstance().GetInt(NXOsSetting::CurrentUser);
    return active >= 0 && static_cast<size_t>(active) < count ? active : 0;
}

void UserProfiles::SetActive(int index)
{
    SettingsStore::GetInstance().SetInt(NXOsSetting::CurrentUser, index);
}
