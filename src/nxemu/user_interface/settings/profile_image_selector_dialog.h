#pragma once
#include <sciter_handler.h>
#include <sciter_ui.h>
#include <vector>

class SystemModules;

struct PendingProfileImage
{
    std::vector<uint8_t> data;

    bool HasImage() const { return !data.empty(); }
};

class ProfileImageSelectorDialog :
    public IClickSink
{
public:
    ProfileImageSelectorDialog(ISciterUI & sciterUI, SystemModules & modules);
    ~ProfileImageSelectorDialog();

    bool Display(void * parentWindow, PendingProfileImage & outSelection);

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

private:
    ProfileImageSelectorDialog() = delete;
    ProfileImageSelectorDialog(const ProfileImageSelectorDialog &) = delete;
    ProfileImageSelectorDialog & operator=(const ProfileImageSelectorDialog &) = delete;

    void Close();
    bool ImportImageFile();
    bool SelectFirmwareAvatar();
    bool HasInstalledFirmware() const;

    ISciterUI & m_sciterUI;
    SystemModules & m_modules;
    ISciterWindow * m_window;
    PendingProfileImage m_selection;
    bool m_changed;
};
