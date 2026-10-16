#pragma once
#include "dialog.h"
#include "settings_page.h"
#include "user_profiles.h"
#include <functional>
#include <nxemu/ui/library/firmware_avatars.h>
#include <string>
#include <vector>

class EmulatorSession;
class UiWindow;

// The page of the settings for the users of the console: who plays, under
// what name and picture. Each has saves of its own. A change is kept as it is
// made.
class ProfilesDialog final : public Dialog
{
public:
    struct Host
    {
        // Whether the profiles may change now: not while a game runs.
        std::function<bool()> canChange;
        // The profiles or the active one are no longer what the window shows.
        std::function<void()> changed;
        // Shows another page of the settings in place of this one.
        OpenSettingsPage openPage;
    };

    ProfilesDialog(UiWindow & window, EmulatorSession & session, const std::filesystem::path & assetDir, Host host);

    // The pictures of the console are still coming: what the dialog shows
    // changes without an event.
    bool Busy() const { return m_avatarsOpen && (m_avatars.Loading() || m_avatarsPending); }
    // The modules are about to be replaced: closes at once.
    void Abandon();

private:
    void OnOpen() override;
    void OnUpdate() override;
    void OnCommand(const std::string & command) override;
    void OnCloseRequested() override;

    void ReadProfiles();
    void ShowProfiles();
    void ShowEditor();
    void Select(int index);
    void AddProfile();
    void RemoveProfile();
    void ChoosePicture();
    void SetPicture(const std::vector<uint8_t> & picture);
    void OpenAvatars();
    void CloseAvatars();
    void ShowAvatars();
    void UseAvatar();
    void SetActive(int index);
    // Gives the profile the name typed, when it is another one.
    void CommitName();
    std::string PictureRml(const UserProfile & profile) const;

    UiWindow & m_window;
    EmulatorSession & m_session;
    const Host m_host;
    std::vector<UserProfile> m_profiles;
    int m_selected = 0;           // the profile the editor shows
    bool m_locked = false;        // a game runs: nothing may change
    bool m_confirmRemove = false; // the editor asks whether to delete the profile
    bool m_commitName = false;    // the name field was left or validated
    bool m_focusName = false;     // the name of a new profile is to be typed
    FirmwareAvatars m_avatars;             // the pictures of the console
    std::vector<std::string> m_avatarFiles; // the ones shown
    bool m_avatarsOpen = false;    // they are shown in place of the profiles
    bool m_avatarsPending = false; // some are read and not shown yet
    int m_avatar = -1;             // the one chosen, in m_avatarFiles
    int m_colour = 0;              // what it is put on, in the colours offered
    bool m_unsaved = false;       // a change is not in the settings file yet
};
