#pragma once
#include <sciter_ui.h>
#include <sciter_element.h>
#include <sciter_handler.h>
#include <widgets/page_nav.h>
#include <nxemu-module-spec/operating_system.h>
#include <string>

class SystemConfig;
class SystemModules;
enum class ProfileEditorMode;

class SystemConfigProfiles :
    public IPagesSink,
    public IClickSink
{
public:
    SystemConfigProfiles(ISciterUI & sciterUI, SystemConfig & config, SystemModules & modules, ISciterWindow & window, SciterElement page);
    ~SystemConfigProfiles() = default;

    void SaveSetting(void);

    // IPagesSink
    bool PageNavChangeFrom(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    bool PageNavChangeTo(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    void PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page) override;
    void PageNavPageChanged(const std::string & pageName, SCITER_ELEMENT pageNav) override;

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

private:
    SystemConfigProfiles() = delete;
    SystemConfigProfiles(const SystemConfigProfiles &) = delete;
    SystemConfigProfiles & operator=(const SystemConfigProfiles &) = delete;

    void SetupProfilesPage(SciterElement page);
    void PopulateProfiles();
    void SelectCurrentUser(int32_t index);
    void AddProfile();
    void OpenEditor(ProfileEditorMode mode, int32_t index = -1);
    std::string GetImageFileUrl(const HostProfileInfo & profile) const;

    ISciterUI & m_sciterUI;
    SystemConfig & m_config;
    SystemModules & m_modules;
    ISciterWindow & m_window;
    SciterElement m_page;
    SciterElement m_profilesPage;
    std::shared_ptr<IPageNav> m_pageNav;
    bool m_enabled;
    int32_t m_currentUser;
};
