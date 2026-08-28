#pragma once
#include "profile_image_selector_dialog.h"
#include <sciter_handler.h>
#include <sciter_ui.h>
#include <nxemu-module-spec/operating_system.h>
#include <stdint.h>

class SystemModules;

enum class ProfileEditorMode
{
    CreateNewProfile,
    EditExistingProfile,
};

class ProfileEditorDialog :
    public IClickSink
{
public:
    ProfileEditorDialog(ISciterUI & sciterUI, SystemModules & modules);
    ~ProfileEditorDialog();

    bool Display(void * parentWindow, ProfileEditorMode mode, int32_t profileIndex = -1);

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

private:
    ProfileEditorDialog() = delete;
    ProfileEditorDialog(const ProfileEditorDialog &) = delete;
    ProfileEditorDialog & operator=(const ProfileEditorDialog &) = delete;

    void Close();
    void UpdateControls();
    void SetDetailImage(const std::string & imageUri);
    bool CommitPendingImage();
    std::string GetUsernameInput() const;
    std::string GetImageFileUrl(const HostProfileInfo & profile) const;

    ISciterUI & m_sciterUI;
    SystemModules & m_modules;
    ISciterWindow * m_window;
    ProfileEditorMode m_mode;
    int32_t m_editingIndex;
    HostProfileInfo m_editingProfile;
    PendingProfileImage m_pendingImage;
    bool m_changed;
};
