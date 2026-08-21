#pragma once

#include <nxemu-module-spec/operating_system.h>
#include <sciter_element.h>
#include <sciter_handler.h>
#include "user_interface/app_events.h"
#include <mutex>

class SystemModules;

class ProfileSelectApplet :
    public IProfileSelectApplet,
    public IEventSink,
    public IClickSink
{
public:
    ProfileSelectApplet();

    void Attach(ISciterUI & sciterUI, SystemModules & modules, SciterElement rootElement, void * parentHwnd);
    void Detach();

    // IProfileSelectApplet
    void Close() override;
    void SelectProfile(void * user_data, ProfileSelectFinishedFn finished, const ProfileSelectHostParameters * parameters) const override;

    // IEventSink
    bool OnEvent(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t event_code, uint64_t reason) override;
    
    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

private:
    ProfileSelectApplet(const ProfileSelectApplet &) = delete;
    ProfileSelectApplet & operator=(const ProfileSelectApplet &) = delete;

    void ProcessPendingSelect();
    void ShowDialog();
    void PopulateProfiles();
    void SelectIndex(int32_t index);
    bool ResolveTileIndex(SCITER_ELEMENT source, int32_t & index) const;
    void AcceptSelection();
    void CancelSelection();
    void CloseDialog();
    void Finish(bool hasUuid, const uint8_t uuidBytes[HOST_PROFILE_UUID_SIZE]);
    std::string GetImageFileUrl(const HostProfileInfo & profile) const;

    static const char * WindowTitleForMode(ProfileUiMode mode);
    static const char * InstructionForPurpose(UserSelectionPurposeHost purpose);

    ISciterUI * m_sciterUI;
    SystemModules * m_modules;
    SciterElement m_rootElement;
    void * m_parentHwnd;
    ISciterWindow * m_window;
    int32_t m_selectedIndex;
    bool m_dialogOpen;

    mutable std::mutex m_mutex;
    mutable bool m_pending;
    mutable void * m_userData;
    mutable ProfileSelectFinishedFn m_finished;
    mutable ProfileSelectHostParameters m_parameters;
};
