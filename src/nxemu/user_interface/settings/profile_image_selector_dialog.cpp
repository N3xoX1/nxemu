#include "profile_image_selector_dialog.h"
#include "profile_firmware_avatar_dialog.h"
#include "user_interface/file_dialogs.h"
#include "user_interface/html_utils.h"
#include <common/path.h>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-module-spec/system_loader.h>
#include <sciter_element.h>
#include <sciter_ui.h>
#include <fstream>

ProfileImageSelectorDialog::ProfileImageSelectorDialog(ISciterUI & sciterUI, SystemModules & modules) :
    m_sciterUI(sciterUI),
    m_modules(modules),
    m_window(nullptr),
    m_selection{},
    m_changed(false)
{
}

ProfileImageSelectorDialog::~ProfileImageSelectorDialog() = default;

bool ProfileImageSelectorDialog::Display(void * parentWindow, PendingProfileImage & outSelection)
{
    enum
    {
        WINDOW_WIDTH = 500,
    };

    m_selection = {};
    m_changed = false;
    m_window = nullptr;
    outSelection = {};

    if (!m_sciterUI.WindowCreate(parentWindow, "profile_image_selector_dialog.html", 0, 0, WINDOW_WIDTH, 0, SUIW_CHILD, m_window))
    {
        return false;
    }

    SciterElement root(m_window->GetRootElement());
    if (root.IsValid())
    {
        SciterElement firmwareButton(root.GetElementByID("profileImageFirmware"));
        if (firmwareButton.IsValid())
        {
            if (HasInstalledFirmware())
            {
                firmwareButton.SetState(0, SciterElement::STATE_DISABLED, true);
            }
            else
            {
                firmwareButton.SetState(SciterElement::STATE_DISABLED, 0, true);
            }
        }

        AttachClickHandler(m_sciterUI, root.GetElementByID("profileImageImport"), this);
        AttachClickHandler(m_sciterUI, root.GetElementByID("profileImageFirmware"), this);
    }

    m_window->FixMinSize();
    m_window->CenterWindow();
    m_window->RunModal();

    if (m_changed)
    {
        outSelection = m_selection;
    }
    return m_changed;
}

bool ProfileImageSelectorDialog::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/, uint32_t /*reason*/)
{
    SciterElement clickElem(element);
    const std::string elementID = clickElem.GetAttributeByName("id");

    if (elementID == "profileImageImport")
    {
        if (ImportImageFile())
        {
            m_changed = true;
            Close();
        }
        return true;
    }

    if (elementID == "profileImageFirmware")
    {
        if (SelectFirmwareAvatar())
        {
            m_changed = true;
            Close();
        }
        return true;
    }

    return true;
}

void ProfileImageSelectorDialog::Close()
{
    if (m_window == nullptr || m_window->IsClosed())
    {
        return;
    }
    m_window->Destroy();
}

bool ProfileImageSelectorDialog::HasInstalledFirmware() const
{
    if (!m_modules.IsValid())
    {
        return false;
    }

    char buffer[32]{};
    return m_modules.Modules().Systemloader().GetInstalledFirmwareDisplayVersion(buffer, sizeof(buffer)) > 0;
}

bool ProfileImageSelectorDialog::ImportImageFile()
{
    Path file;
    const char * filter =
        "Image Files (*.jpg;*.jpeg;*.png;*.bmp)\0*.jpg;*.jpeg;*.png;*.bmp\0"
        "All files (*.*)\0*.*\0";
    if (!FileSelect((void *)(m_window != nullptr ? m_window->GetHandle() : nullptr), Path(Path::CURRENT_DIRECTORY), filter, true, file))
    {
        return false;
    }

    std::ifstream stream(static_cast<const char *>(file), std::ios::binary);
    if (!stream)
    {
        return false;
    }

    m_selection = {};
    m_selection.data.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return m_selection.HasImage();
}

bool ProfileImageSelectorDialog::SelectFirmwareAvatar()
{
    if (!HasInstalledFirmware())
    {
        return false;
    }

    ProfileFirmwareAvatarDialog dialog(m_sciterUI, m_modules);
    PendingProfileImage selection{};
    if (!dialog.Display((void *)(m_window != nullptr ? m_window->GetHandle() : nullptr), selection))
    {
        return false;
    }

    m_selection = selection;
    return true;
}
