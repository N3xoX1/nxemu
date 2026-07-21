#include "profile_editor_dialog.h"
#include "profile_image_selector_dialog.h"
#include "user_interface/html_utils.h"
#include "user_interface/notification.h"
#include <common/std_string.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <sciter_element.h>
#include <sciter_ui.h>
#include <yuzu_common/uuid.h>

namespace
{
std::string FormatUuid(const HostProfileInfo & profile)
{
    std::string uuid_text;
    uuid_text.reserve(36);
    static const char * hex = "0123456789ABCDEF";
    for (uint32_t i = 0; i < HOST_PROFILE_UUID_SIZE; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
        {
            uuid_text.push_back('-');
        }
        uuid_text.push_back(hex[(profile.uuid[i] >> 4) & 0xF]);
        uuid_text.push_back(hex[profile.uuid[i] & 0xF]);
    }
    return uuid_text;
}
}

ProfileEditorDialog::ProfileEditorDialog(ISciterUI & sciterUI, SystemModules & modules) :
    m_sciterUI(sciterUI),
    m_modules(modules),
    m_window(nullptr),
    m_mode(ProfileEditorMode::EditExistingProfile),
    m_editingIndex(-1),
    m_editingProfile{},
    m_pendingImage{},
    m_changed(false)
{
}

ProfileEditorDialog::~ProfileEditorDialog() = default;

bool ProfileEditorDialog::Display(void * parentWindow, ProfileEditorMode mode, int32_t profileIndex)
{
    enum
    {
        WINDOW_WIDTH = 520,
    };

    m_mode = mode;
    m_editingIndex = profileIndex;
    m_editingProfile = {};
    m_pendingImage = {};
    m_changed = false;
    m_window = nullptr;

    if (!m_modules.IsValid())
    {
        return false;
    }

    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    if (m_mode == ProfileEditorMode::CreateNewProfile)
    {
        if (os.GetProfileCount() >= HOST_PROFILE_MAX_USERS)
        {
            Notification::GetInstance().DisplayError("Unable to create profile. A maximum of 8 users is supported.", "Profiles");
            return false;
        }
        const Common::UUID uuid = Common::UUID::MakeRandom();
        std::memcpy(m_editingProfile.uuid, uuid.uuid.data(), HOST_PROFILE_UUID_SIZE);
        m_editingProfile.username[0] = '\0';
        m_editingIndex = -1;
    }
    else if (m_mode == ProfileEditorMode::EditExistingProfile && !os.GetProfile(static_cast<uint32_t>(profileIndex), &m_editingProfile))
    {
        return false;
    }

    if (!m_sciterUI.WindowCreate(parentWindow, "profile_editor_dialog.html", 0, 0, WINDOW_WIDTH, 0, SUIW_CHILD, m_window))
    {
        return false;
    }

    SciterElement root(m_window->GetRootElement());
    if (root.IsValid())
    {
        SciterElement titleEl(root.GetElementByID("ProfileEditorTitle"));
        if (titleEl.IsValid())
        {
            titleEl.SetText(m_mode == ProfileEditorMode::CreateNewProfile ? "Create Profile" : "Edit Profile");
        }

        SciterElement usernameInput(root.GetElementByID("ProfileUsername"));
        SciterElement userIdInput(root.GetElementByID("ProfileUserId"));
        if (usernameInput.IsValid())
        {
            usernameInput.SetValue(SciterValue(std::string(m_editingProfile.username)));
        }
        if (userIdInput.IsValid())
        {
            userIdInput.SetValue(SciterValue(FormatUuid(m_editingProfile)));
        }

        SetDetailImage(m_mode == ProfileEditorMode::CreateNewProfile ? std::string() : GetImageFileUrl(m_editingProfile));
        UpdateControls();

        AttachClickHandler(m_sciterUI, root.GetElementByID("profileRemove"), this);
        AttachClickHandler(m_sciterUI, root.GetElementByID("profileSetImage"), this);
        AttachClickHandler(m_sciterUI, root.GetElementByID("profileSave"), this);
    }

    m_window->FixMinSize();
    m_window->CenterWindow();
    m_window->RunModal();
    return m_changed;
}

bool ProfileEditorDialog::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/, uint32_t /*reason*/)
{
    SciterElement clickElem(element);
    const std::string elementID = clickElem.GetAttributeByName("id");
    if (!m_modules.IsValid())
    {
        return true;
    }

    IOperatingSystem & os = m_modules.Modules().OperatingSystem();

    if (elementID == "profileSave")
    {
        const std::string username = GetUsernameInput();
        if (username.empty())
        {
            Notification::GetInstance().DisplayError("Enter a username for the profile.", "Profiles");
            return true;
        }

        if (m_mode == ProfileEditorMode::CreateNewProfile)
        {
            HostProfileInfo created{};
            if (!os.CreateProfile(m_editingProfile.uuid, username.c_str(), &created))
            {
                Notification::GetInstance().DisplayError("Unable to create profile. A maximum of 8 users is supported.", "Profiles");
                return true;
            }

            m_editingProfile = created;
            m_editingIndex = (int32_t)(os.GetProfileCount() - 1);

            if (m_pendingImage.HasImage() && !CommitPendingImage())
            {
                return true;
            }

            m_mode = ProfileEditorMode::EditExistingProfile;
            m_changed = true;
            Close();
            return true;
        }

        if (!os.RenameProfile(m_editingProfile.uuid, username.c_str()))
        {
            Notification::GetInstance().DisplayError("Unable to rename profile.", "Profiles");
            return true;
        }

        if (m_pendingImage.HasImage() && !CommitPendingImage())
        {
            return true;
        }

        m_changed = true;
        Close();
        return true;
    }

    if (elementID == "profileRemove")
    {
        if (m_mode == ProfileEditorMode::CreateNewProfile)
        {
            return true;
        }

        const std::string message = stdstr_f("Delete user \"%s\"?\n\nSave data for this user will remain on disk.", m_editingProfile.username);
        if (Notification::GetInstance().Query(message.c_str(), "Confirm Delete") != NotificationResponse::Yes)
        {
            return true;
        }

        if (!os.RemoveProfile(m_editingProfile.uuid))
        {
            Notification::GetInstance().DisplayError("Unable to delete profile. At least one user must remain.", "Profiles");
            return true;
        }

        SettingsStore & settings = SettingsStore::GetInstance();
        if (settings.GetInt(NXOsSetting::CurrentUser) == m_editingIndex)
        {
            settings.SetInt(NXOsSetting::CurrentUser, 0);
        }
        else if (settings.GetInt(NXOsSetting::CurrentUser) > m_editingIndex)
        {
            settings.SetInt(NXOsSetting::CurrentUser, settings.GetInt(NXOsSetting::CurrentUser) - 1);
        }

        m_pendingImage = {};
        m_changed = true;
        Close();
        return true;
    }

    if (elementID == "profileSetImage")
    {
        ProfileImageSelectorDialog selector(m_sciterUI, m_modules);
        PendingProfileImage selection{};
        if (selector.Display((void *)(m_window != nullptr ? m_window->GetHandle() : nullptr), selection))
        {
            m_pendingImage = selection;
            SetDetailImage(m_pendingImage.HasImage() ? ImageDataUri(m_pendingImage.data.data(), m_pendingImage.data.size()) : std::string{});
        }
        return true;
    }

    return true;
}

void ProfileEditorDialog::Close()
{
    if (m_window == nullptr || m_window->IsClosed())
    {
        return;
    }
    m_window->Destroy();
}

void ProfileEditorDialog::UpdateControls()
{
    if (m_window == nullptr)
    {
        return;
    }

    SciterElement root(m_window->GetRootElement());
    const uint32_t count = m_modules.IsValid() ? m_modules.Modules().OperatingSystem().GetProfileCount() : 0;
    const bool editing = m_mode == ProfileEditorMode::EditExistingProfile;

    SetElementEnabled(root, "profileSave", true);
    SetElementEnabled(root, "profileSetImage", true);
    SetElementVisible(root, "profileRemove", editing);
    SetElementEnabled(root, "profileRemove", editing && count >= 2);
}

void ProfileEditorDialog::SetDetailImage(const std::string & imageUri)
{
    if (m_window == nullptr)
    {
        return;
    }

    SciterElement image(SciterElement(m_window->GetRootElement()).GetElementByID("ProfileDetailImage"));
    if (!image.IsValid())
    {
        return;
    }

    image.SetAttribute("src", "");
    image.SetStyleAttribute("foreground-image", "none");

    SciterElement avatar = image.GetParent();
    if (avatar.IsValid())
    {
        avatar.SetStyleAttribute("foreground-image", "none");
        avatar.SetStyleAttribute("background", "");
    }

    if (imageUri.empty())
    {
        image.SetStyleAttribute("display", "none");
    }
    else
    {
        const std::string foreground = "url(" + imageUri + ")";
        image.SetAttribute("src", imageUri.c_str());
        image.SetStyleAttribute("foreground-image", foreground.c_str());
        image.SetStyleAttribute("foreground-size", "100% 100%");
        image.SetStyleAttribute("background", "transparent");
        image.SetStyleAttribute("display", "block");

        if (avatar.IsValid())
        {
            avatar.SetStyleAttribute("foreground-image", foreground.c_str());
            avatar.SetStyleAttribute("foreground-size", "100% 100%");
        }
    }
}

bool ProfileEditorDialog::CommitPendingImage()
{
    if (!m_modules.IsValid() || !m_pendingImage.HasImage())
    {
        return true;
    }

    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    if (!os.SetProfileImage(m_editingProfile.uuid, m_pendingImage.data.data(), (uint32_t)m_pendingImage.data.size()))
    {
        Notification::GetInstance().DisplayError("Unable to set profile image.", "Profiles");
        return false;
    }

    m_pendingImage = {};
    return true;
}

std::string ProfileEditorDialog::GetUsernameInput() const
{
    if (m_window == nullptr)
    {
        return {};
    }

    SciterElement input(SciterElement(m_window->GetRootElement()).GetElementByID("ProfileUsername"));
    if (!input.IsValid())
    {
        return {};
    }

    SciterValue value = input.GetValue();
    if (!value.isString())
    {
        return {};
    }

    std::string username = value.GetValueStr();
    if (username.size() > HOST_PROFILE_USERNAME_SIZE)
    {
        username.resize(HOST_PROFILE_USERNAME_SIZE);
    }
    return username;
}

std::string ProfileEditorDialog::GetImageFileUrl(const HostProfileInfo & profile) const
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
