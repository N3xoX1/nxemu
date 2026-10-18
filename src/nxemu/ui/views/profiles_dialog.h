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

class ProfilesDialog final : public Dialog
{
public:
    struct Host
    {
        std::function<bool()> canChange;
        std::function<void()> changed;
        OpenSettingsPage openPage;
    };

    ProfilesDialog(UiWindow & window, EmulatorSession & session, const std::filesystem::path & assetDir, Host host);

    bool Busy() const { return m_avatarsOpen && (m_avatars.Loading() || m_avatarsPending); }
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
    void CommitName();
    std::string PictureRml(const UserProfile & profile) const;

    UiWindow & m_window;
    EmulatorSession & m_session;
    const Host m_host;
    std::vector<UserProfile> m_profiles;
    int m_selected = 0;
    bool m_locked = false;
    bool m_confirmRemove = false;
    bool m_commitName = false;
    bool m_focusName = false;
    FirmwareAvatars m_avatars;
    std::vector<std::string> m_avatarFiles; // the ones shown
    bool m_avatarsOpen = false;    // they are shown in place of the profiles
    bool m_avatarsPending = false; // some are read and not shown yet
    int m_avatar = -1;             // the one chosen, in m_avatarFiles
    int m_colour = 0;              // what it is put on, in the colours offered
    bool m_unsaved = false;       // a change is not in the settings file yet
};
