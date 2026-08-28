#pragma once
#include "profile_image_selector_dialog.h"
#include <sciter_handler.h>
#include <sciter_ui.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class SystemModules;

struct FirmwareProfileAvatar
{
    std::string thumb_path;
    std::string full_path;
};

struct FirmwareAvatarLoadState
{
    std::atomic_bool done{false};
    std::mutex mutex;
    std::vector<FirmwareProfileAvatar> avatars;
};

class ProfileFirmwareAvatarDialog :
    public IClickSink,
    public ITimerSink
{
public:
    ProfileFirmwareAvatarDialog(ISciterUI & sciterUI, SystemModules & modules);
    ~ProfileFirmwareAvatarDialog();

    bool Display(void * parentWindow, PendingProfileImage & outSelection);

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

    // ITimerSink
    bool OnTimer(SCITER_ELEMENT element, uint32_t * timerId) override;

private:
    ProfileFirmwareAvatarDialog() = delete;
    ProfileFirmwareAvatarDialog(const ProfileFirmwareAvatarDialog &) = delete;
    ProfileFirmwareAvatarDialog & operator=(const ProfileFirmwareAvatarDialog &) = delete;

    void Close();
    void StartAvatarLoad();
    void PopulateGridStructure();
    void SyncAvatarsFromLoad();
    void ShowEmptyAvatarsMessage();
    void ApplyNextImageBatch();
    void UpdateSelectionHighlight();
    void ApplyBackgroundColorPreview();
    void SelectBackgroundColor(const std::string & colorCss);
    bool ParseSelectedColor(uint8_t & r, uint8_t & g, uint8_t & b) const;
    bool ChooseSelected();

    ISciterUI & m_sciterUI;
    SystemModules & m_modules;
    ISciterWindow * m_window;
    int32_t m_selectedIndex;
    std::string m_backgroundColor;
    PendingProfileImage m_selection;
    bool m_changed;

    std::vector<FirmwareProfileAvatar> m_avatars;
    std::vector<std::string> m_pngPaths;
    uint32_t m_nextImageIndex;

    std::shared_ptr<FirmwareAvatarLoadState> m_loadState;
};
