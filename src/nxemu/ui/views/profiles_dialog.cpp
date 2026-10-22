#include "profiles_dialog.h"
#include "rml_events.h"
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/StringUtilities.h>
#include <common/path.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/ui_renderer.h>
#include <nxemu/ui/host/ui_window.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/platform/file_dialogs.h>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/uuid.h>

namespace
{
constexpr const char * kSelectPrefix = "select:";
constexpr const char * kAvatarPrefix = "avatar:";
constexpr const char * kColourPrefix = "colour:";
// How many more pictures of the console a frame shows: each is a file to read.
constexpr size_t kAvatarsPerFrame = 8;

// What a picture of the console can be put on.
struct Colour
{
    const char * css;
    uint8_t red, green, blue;
};
constexpr Colour kColours[] = {
    {"#ffffff", 0xFF, 0xFF, 0xFF}, {"#000000", 0x00, 0x00, 0x00}, {"#e60012", 0xE6, 0x00, 0x12}, {"#f4a60a", 0xF4, 0xA6, 0x0A},
    {"#f5e53f", 0xF5, 0xE5, 0x3F}, {"#00a651", 0x00, 0xA6, 0x51}, {"#00a0e9", 0x00, 0xA0, 0xE9}, {"#1d2088", 0x1D, 0x20, 0x88},
    {"#920783", 0x92, 0x07, 0x83}, {"#e4007f", 0xE4, 0x00, 0x7F}, {"#8c8c8c", 0x8C, 0x8C, 0x8C}, {"#c9a26b", 0xC9, 0xA2, 0x6B},
};
constexpr int kColourCount = static_cast<int>(sizeof(kColours) / sizeof(kColours[0]));

// The name as the console keeps it: no longer than it has room for, whole
// characters only, no space around.
std::string CleanName(std::string name)
{
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!name.empty() && isSpace(name.back()))
    {
        name.pop_back();
    }
    size_t start = 0;
    while (start < name.size() && isSpace(name[start]))
    {
        start++;
    }
    name.erase(0, start);
    if (name.size() > HOST_PROFILE_USERNAME_SIZE)
    {
        size_t size = HOST_PROFILE_USERNAME_SIZE;
        while (size > 0 && (static_cast<unsigned char>(name[size]) & 0xC0) == 0x80)
        {
            size--;
        }
        name.resize(size);
    }
    return name;
}

// The first character of the name, shown in place of a picture.
std::string Initial(const std::string & name)
{
    size_t size = name.empty() ? 0 : 1;
    while (size < name.size() && (static_cast<unsigned char>(name[size]) & 0xC0) == 0x80)
    {
        size++;
    }
    return name.substr(0, size);
}

std::vector<uint8_t> ReadFile(const std::string & utf8Path)
{
    std::ifstream file(std::filesystem::path{Common::FS::ToU8String(utf8Path)}, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}
} // namespace

ProfilesDialog::ProfilesDialog(UiWindow & window, EmulatorSession & session, const std::filesystem::path & assetDir, Host host) :
    Dialog(window.Context(), assetDir / "profiles_dialog.rml", kSettingsNarrowBelow),
    m_window(window),
    m_session(session),
    m_host(std::move(host))
{
}

void ProfilesDialog::OnOpen()
{
    m_locked = !m_host.canChange();
    m_confirmRemove = false;
    m_commitName = false;
    m_focusName = false;
    m_avatarsOpen = false;
    m_avatarsPending = false;
    m_avatarFiles.clear();
    ReadProfiles();
    m_selected = UserProfiles::Active(m_profiles.size());
    ShowProfiles();
    ShowEditor();
    Rml::Element * name = Document()->GetElementById("name");
    // Typing only edits the field: the name is given when it is left or validated.
    UiEvents::Listen(name, "blur", [this](Rml::Event &) { m_commitName = true; });
    UiEvents::Listen(name, "change", [this](Rml::Event & event) {
        if (event.GetParameter<bool>("linebreak", false))
        {
            m_commitName = true;
        }
    });
}

void ProfilesDialog::OnUpdate()
{
    if (m_commitName)
    {
        m_commitName = false;
        CommitName();
    }
    if (m_locked == m_host.canChange())
    {
        m_locked = !m_locked;
        m_confirmRemove = false;
        if (m_avatarsOpen)
        {
            CloseAvatars();
        }
        ReadProfiles();
        ShowProfiles();
        ShowEditor();
    }
    if (m_avatarsOpen)
    {
        // A few more of those read at a time: showing one reads its file.
        const std::vector<std::string> files = m_avatars.Files();
        const size_t shown = std::min(files.size(), m_avatarFiles.size() + kAvatarsPerFrame);
        m_avatarsPending = shown < files.size();
        const bool loading = m_avatars.Loading();
        if (shown != m_avatarFiles.size() || loading != Document()->IsClassSet("avatars-loading"))
        {
            m_avatarFiles.assign(files.begin(), files.begin() + static_cast<std::ptrdiff_t>(shown));
            Document()->SetClass("avatars-loading", loading);
            ShowAvatars();
        }
    }
    if (m_focusName)
    {
        m_focusName = false;
        if (auto * name = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("name")))
        {
            name->Focus();
            name->Select();
        }
    }
}

void ProfilesDialog::OnCommand(const std::string & command)
{
    SettingsPage page;
    if (SettingsPageOf(command, page))
    {
        if (m_host.openPage(page))
        {
            OnCloseRequested();
        }
        return;
    }
    if (m_locked)
    {
        return;
    }
    if (command.rfind(kSelectPrefix, 0) == 0)
    {
        Select(std::atoi(command.c_str() + std::strlen(kSelectPrefix)));
    }
    else if (command == "add")
    {
        AddProfile();
    }
    else if (command == "use")
    {
        SetActive(m_selected);
    }
    else if (command == "picture")
    {
        ChoosePicture();
    }
    else if (command == "avatars")
    {
        OpenAvatars();
    }
    else if (command == "avatars-back")
    {
        CloseAvatars();
    }
    else if (command == "avatar-use")
    {
        UseAvatar();
    }
    else if (command.rfind(kAvatarPrefix, 0) == 0)
    {
        m_avatar = std::atoi(command.c_str() + std::strlen(kAvatarPrefix));
        ShowAvatars();
    }
    else if (command.rfind(kColourPrefix, 0) == 0)
    {
        m_colour = std::min(std::max(std::atoi(command.c_str() + std::strlen(kColourPrefix)), 0), kColourCount - 1);
        ShowAvatars();
    }
    else if (command == "remove" || command == "keep")
    {
        m_confirmRemove = command == "remove";
        ShowEditor();
    }
    else if (command == "remove-confirmed")
    {
        RemoveProfile();
    }
}

void ProfilesDialog::Abandon()
{
    m_avatars.Wait();
    if (IsVisible())
    {
        if (m_avatarsOpen)
        {
            CloseAvatars();
        }
        Close();
    }
}

void ProfilesDialog::OnCloseRequested()
{
    if (m_avatarsOpen)
    {
        CloseAvatars();
    }
    CommitName();
    if (m_unsaved)
    {
        m_unsaved = false;
        m_session.FlushSettings();
    }
    Close();
}

void ProfilesDialog::ReadProfiles()
{
    m_profiles = m_session.IsValid() ? UserProfiles::List(m_session.Modules().OperatingSystem()) : std::vector<UserProfile>();
    m_selected = m_profiles.empty() ? 0 : std::min(std::max(m_selected, 0), static_cast<int>(m_profiles.size()) - 1);
}

std::string ProfilesDialog::PictureRml(const UserProfile & profile) const
{
    if (profile.picture.empty())
    {
        return "<div class=\"picture-initial\">" + Rml::StringUtilities::EncodeRml(Initial(profile.name)) + "</div>";
    }
    return "<img src=\"" + Rml::StringUtilities::EncodeRml(UiRenderer::FileImageSource(profile.picture)) + "\"/>";
}

void ProfilesDialog::ShowProfiles()
{
    const int active = UserProfiles::Active(m_profiles.size());
    std::string rml;
    for (int index = 0; index < static_cast<int>(m_profiles.size()); index++)
    {
        rml += std::string("<div class=\"profile") + (index == m_selected ? " selected" : "") + (index == active ? " active" : "") +
               "\" data-command=\"" + kSelectPrefix + std::to_string(index) + "\"><div class=\"picture\">" + PictureRml(m_profiles[index]) +
               "</div><div class=\"profile-name\">" + Rml::StringUtilities::EncodeRml(m_profiles[index].name) +
               "</div><div class=\"profile-active\">" + Rml::StringUtilities::EncodeRml(Tr("Active")) + "</div></div>";
    }
    if (!m_locked && m_profiles.size() < HOST_PROFILE_MAX_USERS)
    {
        rml += "<div class=\"profile add\" data-command=\"add\" title=\"" + Rml::StringUtilities::EncodeRml(Tr("A new player, with saves of their own")) +
               "\"><div class=\"picture\"><div class=\"picture-initial\">+</div></div><div class=\"profile-name\">" + Rml::StringUtilities::EncodeRml(Tr("New profile")) + "</div></div>";
    }
    SetText("profiles", rml);
    Document()->SetClass("profiles-locked", m_locked);
}

void ProfilesDialog::ShowEditor()
{
    Document()->SetClass("no-profile", m_profiles.empty());
    if (m_profiles.empty())
    {
        return;
    }
    const UserProfile & profile = m_profiles[m_selected];
    SetText("picture", PictureRml(profile));
    if (auto * name = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("name")))
    {
        if (name->GetValue() != profile.name)
        {
            name->SetValue(profile.name);
        }
        if (m_locked)
        {
            name->SetAttribute("disabled", "");
        }
        else
        {
            name->RemoveAttribute("disabled");
        }
    }
    Document()->SetClass("active-selected", m_selected == UserProfiles::Active(m_profiles.size()));
    Document()->SetClass("only-profile", m_profiles.size() < 2);
    Document()->SetClass("confirm-remove", m_confirmRemove);
    SetText("remove-question", Rml::StringUtilities::EncodeRml(Tr("Delete %1? Its saves stay on the disk.", {profile.name})));
}

void ProfilesDialog::Select(int index)
{
    if (index < 0 || index >= static_cast<int>(m_profiles.size()) || index == m_selected)
    {
        return;
    }
    CommitName();
    m_selected = index;
    m_confirmRemove = false;
    ShowProfiles();
    ShowEditor();
}

void ProfilesDialog::AddProfile()
{
    if (!m_session.IsValid() || m_profiles.size() >= HOST_PROFILE_MAX_USERS)
    {
        return;
    }
    CommitName();
    // The first "Player N" nobody has.
    std::string name;
    for (int number = static_cast<int>(m_profiles.size()) + 1; name.empty(); number++)
    {
        name = Tr("Player %1", {std::to_string(number)});
        for (const UserProfile & profile : m_profiles)
        {
            if (profile.name == name)
            {
                name.clear();
                break;
            }
        }
    }
    const Common::UUID id = Common::UUID::MakeRandom();
    if (!m_session.Modules().OperatingSystem().CreateProfile(id.uuid.data(), name.c_str(), nullptr))
    {
        return;
    }
    ReadProfiles();
    m_selected = static_cast<int>(m_profiles.size()) - 1;
    m_confirmRemove = false;
    m_focusName = true;
    ShowProfiles();
    ShowEditor();
    m_host.changed();
}

void ProfilesDialog::RemoveProfile()
{
    if (!m_session.IsValid() || m_profiles.size() < 2)
    {
        return;
    }
    const int active = UserProfiles::Active(m_profiles.size());
    if (!m_session.Modules().OperatingSystem().RemoveProfile(m_profiles[m_selected].info.uuid))
    {
        return;
    }
    // The ones after it moved up by one place.
    if (active == m_selected)
    {
        UserProfiles::SetActive(0);
        m_unsaved = true;
    }
    else if (active > m_selected)
    {
        UserProfiles::SetActive(active - 1);
        m_unsaved = true;
    }
    m_confirmRemove = false;
    ReadProfiles();
    ShowProfiles();
    ShowEditor();
    m_host.changed();
}

void ProfilesDialog::ChoosePicture()
{
    if (!m_session.IsValid() || m_profiles.empty())
    {
        return;
    }
    const std::string filter = FileFilter({{Tr("Pictures"), "*.png;*.jpg;*.jpeg;*.bmp"}});
    Path selected;
    if (!FileSelect(m_window.NativeHandle(), Path(Path::CURRENT_DIRECTORY), filter.c_str(), true, selected))
    {
        return;
    }
    SetPicture(ReadFile(static_cast<const char *>(selected)));
}

void ProfilesDialog::SetPicture(const std::vector<uint8_t> & picture)
{
    if (picture.empty() || !m_session.Modules().OperatingSystem().SetProfileImage(m_profiles[m_selected].info.uuid, picture.data(), static_cast<uint32_t>(picture.size())))
    {
        Document()->SetClass("picture-refused", true);
        return;
    }
    Document()->SetClass("picture-refused", false);
    // The file is the same one, with another picture in it.
    if (!m_profiles[m_selected].picture.empty())
    {
        Rml::ReleaseTexture(m_profiles[m_selected].picture);
    }
    ReadProfiles();
    Rml::ReleaseTexture(m_profiles[m_selected].picture);
    ShowProfiles();
    ShowEditor();
    m_host.changed();
}

void ProfilesDialog::OpenAvatars()
{
    if (!m_session.IsValid() || m_profiles.empty())
    {
        return;
    }
    CommitName();
    m_avatars.Load(m_session.Modules().Systemloader());
    m_avatarsOpen = true;
    m_avatarsPending = true;
    m_avatarFiles.clear();
    m_avatar = -1;
    m_confirmRemove = false;
    Document()->SetClass("picture-refused", false);
    Document()->SetClass("avatars-open", true);
    Document()->SetClass("avatars-loading", m_avatars.Loading());
    ShowEditor();
    ShowAvatars();
}

void ProfilesDialog::CloseAvatars()
{
    m_avatarsOpen = false;
    m_avatarsPending = false;
    Document()->SetClass("avatars-open", false);
    SetText("avatars", {});
    // Their textures are of no use until they are shown again.
    for (const std::string & file : m_avatarFiles)
    {
        Rml::ReleaseTexture(file);
    }
    m_avatarFiles.clear();
    m_avatar = -1;
    if (Rml::Element * picture = Document()->GetElementById("picture"))
    {
        picture->RemoveProperty("background-color");
    }
    ShowEditor();
}

void ProfilesDialog::ShowAvatars()
{
    const std::string colour = kColours[m_colour].css;
    std::string rml;
    for (int index = 0; index < static_cast<int>(m_avatarFiles.size()); index++)
    {
        rml += std::string("<div class=\"avatar") + (index == m_avatar ? " selected" : "") + "\" data-command=\"" + kAvatarPrefix + std::to_string(index) +
               "\"><div class=\"picture\" style=\"background-color: " + colour + ";\"><img src=\"" +
               Rml::StringUtilities::EncodeRml(UiRenderer::FileImageSource(m_avatarFiles[index])) + "\"/></div></div>";
    }
    SetText("avatars", rml);
    Document()->SetClass("no-avatar", m_avatarFiles.empty() && !m_avatars.Loading() && !m_avatarsPending);
    Document()->SetClass("avatars-waiting", m_avatarFiles.empty() && (m_avatars.Loading() || m_avatarsPending));
    Document()->SetClass("avatar-chosen", m_avatar >= 0);

    rml.clear();
    for (int index = 0; index < kColourCount; index++)
    {
        rml += std::string("<div class=\"colour") + (index == m_colour ? " selected" : "") + "\" data-command=\"" + kColourPrefix + std::to_string(index) +
               "\" style=\"background-color: " + kColours[index].css + ";\"/>";
    }
    SetText("colours", rml);

    // The editor shows what the profile would look like.
    if (m_avatar >= 0 && m_avatar < static_cast<int>(m_avatarFiles.size()))
    {
        SetText("picture", "<img src=\"" + Rml::StringUtilities::EncodeRml(UiRenderer::FileImageSource(m_avatarFiles[m_avatar])) + "\"/>");
        Document()->GetElementById("picture")->SetProperty("background-color", colour);
    }
}

void ProfilesDialog::UseAvatar()
{
    if (!m_session.IsValid() || m_profiles.empty() || m_avatar < 0 || m_avatar >= static_cast<int>(m_avatarFiles.size()))
    {
        return;
    }
    const Colour & colour = kColours[m_colour];
    const std::vector<uint8_t> picture = FirmwareAvatars::OnColour(m_avatarFiles[m_avatar], colour.red, colour.green, colour.blue);
    CloseAvatars();
    SetPicture(picture);
}

void ProfilesDialog::SetActive(int index)
{
    if (index < 0 || index >= static_cast<int>(m_profiles.size()))
    {
        return;
    }
    UserProfiles::SetActive(index);
    m_unsaved = true;
    ShowProfiles();
    ShowEditor();
    m_host.changed();
}

void ProfilesDialog::CommitName()
{
    auto * field = IsVisible() ? rmlui_dynamic_cast<Rml::ElementFormControlInput *>(Document()->GetElementById("name")) : nullptr;
    if (field == nullptr || m_locked || !m_session.IsValid() || m_profiles.empty())
    {
        return;
    }
    UserProfile & profile = m_profiles[m_selected];
    const std::string name = CleanName(field->GetValue());
    if (name.empty() || name == profile.name || !m_session.Modules().OperatingSystem().RenameProfile(profile.info.uuid, name.c_str()))
    {
        // A profile keeps the name it had rather than none.
        if (field->GetValue() != profile.name)
        {
            field->SetValue(profile.name);
        }
        return;
    }
    profile.name = name;
    field->SetValue(name);
    ShowProfiles();
    ShowEditor();
    m_host.changed();
}
