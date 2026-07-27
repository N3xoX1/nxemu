#include "profile_select.h"

#include "user_interface/html_utils.h"
#include <common/std_string.h>
#include <nxemu-core/modules/system_modules.h>

ProfileSelectApplet::ProfileSelectApplet() :
    m_sciterUI(nullptr),
    m_modules(nullptr),
    m_parentHwnd(nullptr),
    m_window(nullptr),
    m_selectedIndex(0),
    m_dialogOpen(false),
    m_pending(false),
    m_userData(nullptr),
    m_finished(nullptr),
    m_parameters{}
{
}

void ProfileSelectApplet::Attach(ISciterUI & sciterUI, SystemModules & modules, SciterElement rootElement, void * parentHwnd)
{
    m_sciterUI = &sciterUI;
    m_modules = &modules;
    m_rootElement = rootElement;
    m_parentHwnd = parentHwnd;

    if (m_rootElement.IsValid())
    {
        m_sciterUI->AttachHandler(m_rootElement, IID_EVENTSINK, (IEventSink *)this);
    }
}

void ProfileSelectApplet::Detach()
{
    CloseDialog();

    if (m_sciterUI != nullptr && m_rootElement.IsValid())
    {
        m_sciterUI->DetachHandler(m_rootElement, IID_EVENTSINK, (IEventSink *)this);
    }

    m_sciterUI = nullptr;
    m_modules = nullptr;
    m_rootElement = {};
    m_parentHwnd = nullptr;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending = false;
    m_userData = nullptr;
    m_finished = nullptr;
    m_parameters = {};
}

void ProfileSelectApplet::Close()
{
    ProfileSelectFinishedFn finished = nullptr;
    void * userData = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        finished = m_finished;
        userData = m_userData;
        m_pending = false;
        m_finished = nullptr;
        m_userData = nullptr;
    }

    CloseDialog();

    if (finished != nullptr)
    {
        uint8_t uuidBytes[HOST_PROFILE_UUID_SIZE]{};
        finished(userData, false, uuidBytes);
    }
}

void ProfileSelectApplet::SelectProfile(void * user_data, ProfileSelectFinishedFn finished, const ProfileSelectHostParameters * parameters) const
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_userData = user_data;
        m_finished = finished;
        m_parameters = parameters != nullptr ? *parameters : ProfileSelectHostParameters{};
        m_pending = true;
    }

    if (m_rootElement.IsValid())
    {
        m_rootElement.PostEvent(EVENT_PROFILE_SELECT);
    }
    else if (finished != nullptr)
    {
        uint8_t uuidBytes[HOST_PROFILE_UUID_SIZE]{};
        finished(user_data, false, uuidBytes);
    }
}

void ProfileSelectApplet::ProcessPendingSelect()
{
    bool pending = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pending = m_pending;
        m_pending = false;
    }

    if (!pending)
    {
        return;
    }

    ShowDialog();
}

bool ProfileSelectApplet::OnEvent(SCITER_ELEMENT /*element*/, SCITER_ELEMENT /*source*/, uint32_t event_code, uint64_t /*reason*/)
{
    if (event_code == EVENT_PROFILE_SELECT)
    {
        ProcessPendingSelect();
        return true;
    }
    return false;
}

bool ProfileSelectApplet::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t /*reason*/)
{
    SciterElement clickElem(element);
    const std::string elementId = clickElem.GetAttributeByName("id");
    
    if (elementId == "profileSelectOk")
    {
        AcceptSelection();
        return true;
    }
    else if (elementId == "profileSelectCancel")
    {
        CancelSelection();
        return true;
    }

    int32_t index = -1;
    if (ResolveTileIndex(source, index))
    {
        SelectIndex(index);
        return true;
    }
    return true;
}

bool ProfileSelectApplet::ResolveTileIndex(SCITER_ELEMENT source, int32_t & index) const
{
    SciterElement elem(source);
    for (SciterElement walk = elem; walk.IsValid(); walk = walk.GetParent())
    {
        if (walk.GetAttribute("role") == "profile-tile")
        {
            const std::string indexAttr = walk.GetAttribute("data-index");
            if (indexAttr.empty())
            {
                return false;
            }
            index = std::stoi(indexAttr);
            return true;
        }
    }
    return false;
}

void ProfileSelectApplet::ShowDialog()
{
    if (m_sciterUI == nullptr || m_modules == nullptr || !m_modules->IsValid() || m_dialogOpen)
    {
        CancelSelection();
        return;
    }

    IOperatingSystem & os = m_modules->Modules().OperatingSystem();
    const uint32_t count = os.GetProfileCount();
    if (count == 0)
    {
        CancelSelection();
        return;
    }

    if (count == 1)
    {
        HostProfileInfo profile{};
        if (os.GetProfile(0, &profile))
        {
            Finish(true, profile.uuid);
        }
        else
        {
            CancelSelection();
        }
        return;
    }

    enum
    {
        WINDOW_WIDTH = 560,
        WINDOW_HEIGHT = 420,
    };

    m_selectedIndex = 0;
    m_window = nullptr;
    if (!m_sciterUI->WindowCreate(m_parentHwnd, "profile_select_dialog.html", 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, SUIW_CHILD, m_window))
    {
        CancelSelection();
        return;
    }

    m_dialogOpen = true;

    SciterElement root(m_window->GetRootElement());
    if (root.IsValid())
    {
        ProfileSelectHostParameters parameters{};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            parameters = m_parameters;
        }

        SciterElement titleEl(root.GetElementByID("ProfileSelectTitle"));
        if (titleEl.IsValid())
        {
            titleEl.SetText(WindowTitleForMode(parameters.mode));
        }

        SciterElement instructionEl(root.GetElementByID("ProfileSelectInstruction"));
        if (instructionEl.IsValid())
        {
            instructionEl.SetText(InstructionForPurpose(parameters.purpose));
        }

        PopulateProfiles();
        AttachClickHandler(*m_sciterUI, root.GetElementByID("profileSelectOk"), this);
        AttachClickHandler(*m_sciterUI, root.GetElementByID("profileSelectCancel"), this);
    }

    m_window->CenterWindow();
    m_window->RunModal();
    m_dialogOpen = false;
    m_window = nullptr;

    bool needsCancel = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        needsCancel = m_finished != nullptr;
    }
    if (needsCancel)
    {
        CancelSelection();
    }
}

void ProfileSelectApplet::PopulateProfiles()
{
    if (m_window == nullptr || m_sciterUI == nullptr || m_modules == nullptr || !m_modules->IsValid())
    {
        return;
    }

    SciterElement root(m_window->GetRootElement());
    SciterElement grid = root.IsValid() ? root.GetElementByID("profileSelectGrid") : nullptr;
    if (!grid.IsValid())
    {
        return;
    }

    grid.Clear();

    IOperatingSystem & os = m_modules->Modules().OperatingSystem();
    const uint32_t count = os.GetProfileCount();
    if (m_selectedIndex < 0 || (uint32_t)m_selectedIndex >= count)
    {
        m_selectedIndex = 0;
    }

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

        const bool selected = (int32_t)i == m_selectedIndex;
        const std::string imageUri = GetImageFileUrl(profile);
        imageUris.push_back(imageUri);

        html += stdstr_f(
            "<div class=\"profile-tile%s\" role=\"profile-tile\" data-index=\"%u\">"
            "<div class=\"profile-tile-avatar\"></div>"
            "<div class=\"profile-tile-name\">%s</div>"
            "</div>",
            selected ? " current" : "",
            i,
            HtmlEscape(profile.username).c_str());
    }

    grid.SetHTML((const uint8_t *)html.c_str(), html.size());
    m_sciterUI->AttachHandler(grid, IID_ICLICKSINK, (IClickSink *)this);

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

void ProfileSelectApplet::SelectIndex(int32_t index)
{
    if (index == m_selectedIndex || m_window == nullptr)
    {
        return;
    }

    SciterElement root(m_window->GetRootElement());
    SciterElement grid = root.IsValid() ? root.GetElementByID("profileSelectGrid") : nullptr;
    if (!grid.IsValid())
    {
        return;
    }

    SciterElement newTile = grid.FindFirst(stdstr_f("[data-index='%d']", index).c_str());
    if (!newTile.IsValid())
    {
        return;
    }

    SciterElement oldTile = grid.FindFirst(stdstr_f("[data-index='%d']", m_selectedIndex).c_str());
    if (oldTile.IsValid())
    {
        oldTile.SetAttribute("class", "profile-tile");
    }

    newTile.SetAttribute("class", "profile-tile current");
    m_selectedIndex = index;
}

void ProfileSelectApplet::AcceptSelection()
{
    if (m_modules == nullptr || !m_modules->IsValid())
    {
        CancelSelection();
        return;
    }

    HostProfileInfo profile{};
    if (!m_modules->Modules().OperatingSystem().GetProfile((uint32_t)m_selectedIndex, &profile))
    {
        CancelSelection();
        return;
    }

    const HostProfileInfo selected = profile;
    CloseDialog();
    Finish(true, selected.uuid);
}

void ProfileSelectApplet::CancelSelection()
{
    CloseDialog();
    uint8_t uuidBytes[HOST_PROFILE_UUID_SIZE]{};
    Finish(false, uuidBytes);
}

void ProfileSelectApplet::CloseDialog()
{
    if (m_window == nullptr || m_window->IsClosed())
    {
        m_window = nullptr;
        m_dialogOpen = false;
        return;
    }

    m_dialogOpen = false;
    m_window->Destroy();
    m_window = nullptr;
}

void ProfileSelectApplet::Finish(bool hasUuid, const uint8_t uuidBytes[HOST_PROFILE_UUID_SIZE])
{
    ProfileSelectFinishedFn finished = nullptr;
    void * userData = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        finished = m_finished;
        userData = m_userData;
        m_finished = nullptr;
        m_userData = nullptr;
        m_pending = false;
    }

    if (finished != nullptr)
    {
        finished(userData, hasUuid, uuidBytes);
    }
}

std::string ProfileSelectApplet::GetImageFileUrl(const HostProfileInfo & profile) const
{
    if (m_modules == nullptr || !m_modules->IsValid())
    {
        return {};
    }

    char path[1024] = {};
    if (!m_modules->Modules().OperatingSystem().GetProfileImagePath(profile.uuid, path, sizeof(path)) || path[0] == '\0')
    {
        return {};
    }

    return ImageDataUriFromFile(path);
}

const char * ProfileSelectApplet::WindowTitleForMode(ProfileUiMode mode)
{
    switch (mode)
    {
    case ProfileUiMode::UserCreator:
    case ProfileUiMode::UserCreatorForStarter:
        return "Profile Creator";
    case ProfileUiMode::UserIconEditor:
        return "Profile Icon Editor";
    case ProfileUiMode::UserNicknameEditor:
        return "Profile Nickname Editor";
    default:
        return "Profile Selector";
    }
}

const char * ProfileSelectApplet::InstructionForPurpose(UserSelectionPurposeHost purpose)
{
    switch (purpose)
    {
    case UserSelectionPurposeHost::GameCardRegistration:
        return "Who will receive the points?";
    case UserSelectionPurposeHost::EShopLaunch:
        return "Who is using Nintendo eShop?";
    case UserSelectionPurposeHost::EShopItemShow:
        return "Who is making this purchase?";
    case UserSelectionPurposeHost::PicturePost:
        return "Who is posting?";
    case UserSelectionPurposeHost::NintendoAccountLinkage:
        return "Select a user to link to a Nintendo Account.";
    case UserSelectionPurposeHost::SettingsUpdate:
        return "Change settings for which user?";
    case UserSelectionPurposeHost::SaveDataDeletion:
        return "Format data for which user?";
    case UserSelectionPurposeHost::UserMigration:
        return "Which user will be transferred to another console?";
    case UserSelectionPurposeHost::SaveDataTransfer:
        return "Send save data for which user?";
    case UserSelectionPurposeHost::General:
    default:
        return "Select a user:";
    }
}
