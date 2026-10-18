#pragma once
#include <functional>
#include <string>

enum class SettingsPage
{
    General,
    Profiles,
    Directories,
    Advanced,
};

using OpenSettingsPage = std::function<bool(SettingsPage)>;

inline bool SettingsPageOf(const std::string & command, SettingsPage & page)
{
    if (command == "page:general")
    {
        page = SettingsPage::General;
    }
    else if (command == "page:profiles")
    {
        page = SettingsPage::Profiles;
    }
    else if (command == "page:directories")
    {
        page = SettingsPage::Directories;
    }
    else if (command == "page:advanced")
    {
        page = SettingsPage::Advanced;
    }
    else
    {
        return false;
    }
    return true;
}
