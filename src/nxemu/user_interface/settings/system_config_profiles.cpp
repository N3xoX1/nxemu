#include "system_config_profiles.h"
#include "profile_editor_dialog.h"
#include "user_interface/html_utils.h"
#include "user_interface/notification.h"
#include <common/std_string.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>

SystemConfigProfiles::SystemConfigProfiles(ISciterUI & sciterUI, SystemConfig & config, SystemModules & modules, ISciterWindow & window, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_modules(modules),
    m_window(window),
    m_page(page),
    m_enabled(!SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning)),
    m_currentUser(SettingsStore::GetInstance().GetInt(NXOsSetting::CurrentUser))
{
    SciterElement pageNav = page.GetElementByID("ProfilesTabNav");
    std::shared_ptr<void> interfacePtr = pageNav.IsValid() ? m_sciterUI.GetElementInterface(pageNav, IID_IPAGENAV) : nullptr;
    if (interfacePtr)
    {
        m_pageNav = std::static_pointer_cast<IPageNav>(interfacePtr);
        m_pageNav->AddSink(this);
    }
}

void SystemConfigProfiles::SaveSetting(void)
{
    SettingsStore::GetInstance().SetInt(NXOsSetting::CurrentUser, m_currentUser);
}

bool SystemConfigProfiles::PageNavChangeFrom(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

bool SystemConfigProfiles::PageNavChangeTo(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

void SystemConfigProfiles::PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page)
{
    if (pageName == "ManageProfiles")
    {
        SetupProfilesPage(page);
    }
}

void SystemConfigProfiles::PageNavPageChanged(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
}

void SystemConfigProfiles::SetupProfilesPage(SciterElement page)
{
    m_profilesPage = page;
    PopulateProfiles();
}

bool SystemConfigProfiles::OnClick(SCITER_ELEMENT /*element*/, SCITER_ELEMENT source, uint32_t /*reason*/)
{
    SciterElement clickElem(source);
    std::string role;
    std::string indexAttr;

    SciterElement walk = clickElem;
    while (walk.IsValid() && walk != m_page)
    {
        const std::string walkRole = walk.GetAttribute("role");
        const std::string walkIndex = walk.GetAttribute("data-index");

        if (walkRole == "profile-edit" || walkRole == "profile-add" || walkRole == "profile-tile")
        {
            role = walkRole;
            if (!walkIndex.empty())
            {
                indexAttr = walkIndex;
            }
            break;
        }

        if (indexAttr.empty() && !walkIndex.empty())
        {
            indexAttr = walkIndex;
        }
        walk = walk.GetParent();
    }

    if (role == "profile-add")
    {
        if (m_enabled)
        {
            AddProfile();
        }
        return true;
    }

    if (role == "profile-edit")
    {
        if (m_enabled && !indexAttr.empty())
        {
            OpenEditor(ProfileEditorMode::EditExistingProfile, std::stoi(indexAttr));
        }
        return true;
    }

    if (role == "profile-tile")
    {
        if (!indexAttr.empty())
        {
            SelectCurrentUser(std::stoi(indexAttr));
            PopulateProfiles();
        }
        return true;
    }

    return true;
}

void SystemConfigProfiles::AddProfile()
{
    if (!m_modules.IsValid())
    {
        return;
    }

    if (m_modules.Modules().OperatingSystem().GetProfileCount() >= HOST_PROFILE_MAX_USERS)
    {
        Notification::GetInstance().DisplayError("Unable to create profile. A maximum of 8 users is supported.", "Profiles");
        return;
    }

    OpenEditor(ProfileEditorMode::CreateNewProfile);
}

void SystemConfigProfiles::OpenEditor(ProfileEditorMode mode, int32_t index)
{
    if (!m_modules.IsValid())
    {
        return;
    }

    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    const uint32_t countBefore = os.GetProfileCount();

    ProfileEditorDialog dialog(m_sciterUI, m_modules);
    if (!dialog.Display((void *)m_window.GetHandle(), mode, index))
    {
        return;
    }

    const uint32_t count = os.GetProfileCount();
    if (mode == ProfileEditorMode::CreateNewProfile && count > countBefore)
    {
        m_currentUser = static_cast<int32_t>(count - 1);
    }
    else if (count < countBefore)
    {
        m_currentUser = SettingsStore::GetInstance().GetInt(NXOsSetting::CurrentUser);
    }
    PopulateProfiles();
}

void SystemConfigProfiles::PopulateProfiles()
{
    SciterElement grid = m_profilesPage.IsValid() ? m_profilesPage.GetElementByID("profileGrid") : nullptr;
    if (!m_profilesPage.IsValid() || !grid.IsValid() || !m_modules.IsValid())
    {
        return;
    }

    grid.Clear();

    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    const uint32_t count = os.GetProfileCount();
    if (count == 0)
    {
        m_currentUser = 0;
    }
    else if (m_currentUser < 0 || (uint32_t)m_currentUser >= count)
    {
        m_currentUser = 0;
    }

    const int32_t current = m_currentUser;
    std::string activeName;

    std::string html;
    std::vector<std::string> imageUris;
    imageUris.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        HostProfileInfo profile{};
        if (!os.GetProfile(i, &profile))
        {
            continue;
        }

        const bool isCurrent = (int32_t)i == current;
        const std::string imageUri = GetImageFileUrl(profile);
        const std::string name = HtmlEscape(profile.username);
        imageUris.push_back(imageUri);

        if (isCurrent)
        {
            activeName = profile.username;
        }

        html += stdstr_f(
            "<div class=\"profile-tile%s\" role=\"profile-tile\" data-index=\"%u\">"
            "<div class=\"profile-tile-avatar\"></div>"
            "<div class=\"profile-tile-name\">%s</div>"
            "<div class=\"profile-tile-edit\" role=\"profile-edit\" data-index=\"%u\" %s>"
            "<svg viewBox=\"0 0 24 24\">"
            "<path fill=\"none\" stroke-linecap=\"round\" stroke-linejoin=\"round\" stroke-width=\"1.5\" d=\"m18 10 3-3-4-4-3 3m4 4L8 20H4v-4L14 6m4 4-4-4\"/>"
            "</svg>"
            "</div>"
            "</div>",
            isCurrent ? " current" : "",
            i,
            name.c_str(),
            i,
            m_enabled ? "" : "disabled");
    }

    SciterElement activeLabel = m_profilesPage.GetElementByID("ActiveProfileLabel");
    if (activeLabel.IsValid())
    {
        activeLabel.SetText(stdstr_f("Active profile: %s", !activeName.empty() ? activeName.c_str() : "").c_str());
    }

    if (m_enabled && count < HOST_PROFILE_MAX_USERS)
    {
        html +=
            "<div class=\"profile-tile-add\" role=\"profile-add\">"
            "<div class=\"profile-tile-add-button\" role=\"profile-add\">+</div>"
            "</div>";
    }

    grid.SetHTML((const uint8_t *)html.c_str(), html.size());
    m_sciterUI.AttachHandler(grid, IID_ICLICKSINK, (IClickSink *)this);

    for (uint32_t i = 0; i < grid.GetChildCount() && i < imageUris.size(); ++i)
    {
        if (imageUris[i].empty())
        {
            continue;
        }

        SciterElement tile = grid.GetChild(i);
        if (!tile.IsValid())
        {
            continue;
        }

        SciterElement avatar = tile.FindFirst(".profile-tile-avatar");
        if (avatar.IsValid())
        {
            avatar.SetStyleAttribute("foreground-image", ("url(" + imageUris[i] + ")").c_str());
            avatar.SetStyleAttribute("foreground-size", "100% 100%");
            avatar.SetStyleAttribute("foreground-repeat", "no-repeat");
            avatar.SetStyleAttribute("foreground-position", "50% 50%");
        }
    }
}

void SystemConfigProfiles::SelectCurrentUser(int32_t index)
{
    if (index < 0)
    {
        return;
    }
    m_currentUser = index;
}

std::string SystemConfigProfiles::GetImageFileUrl(const HostProfileInfo & profile) const
{
    if (!m_modules.IsValid())
    {
        return {};
    }

    char path[1024] = {};
    if (!m_modules.Modules().OperatingSystem().GetProfileImagePath(profile.uuid, path, sizeof(path)) || path[0] == '\0')
    {
        return {};
    }
    return ImageDataUriFromFile(path);
}
