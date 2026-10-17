#pragma once
#include <functional>
#include <string>

// The pages of the settings. Each is a dialog of its own, with the same
// categories on its left: choosing one shows its page in place of the other.
enum class SettingsPage
{
    General,
    Profiles,
    Directories,
    Advanced,
    Debug,
};

// Shows a page of the settings. False when it cannot be shown now.
using OpenSettingsPage = std::function<bool(SettingsPage)>;

// The page a category names with its command, "page:general" and the like.
// False when the command is not one of a category.
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
    else if (command == "page:debug")
    {
        page = SettingsPage::Debug;
    }
    else
    {
        return false;
    }
    return true;
}
