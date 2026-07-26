#pragma once
#include <sciter_ui.h>
#include <sciter_element.h>
#include <widgets/page_nav.h>

class SystemConfig;
class SystemConfigHotkeys;
class SystemConfigLogging;
class SystemConfigSystem;

class SystemConfigGeneral :
    public IPagesSink
{
public:
    SystemConfigGeneral(ISciterUI & sciterUI, SystemConfig & config, ISciterWindow & window, SciterElement page);
    ~SystemConfigGeneral();

    void SaveSetting(void);
    void SetInitialPage(const char * path);

    // IPagesSink
    bool PageNavChangeFrom(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    bool PageNavChangeTo(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    void PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page) override;
    void PageNavPageChanged(const std::string & pageName, SCITER_ELEMENT pageNav) override;

private:
    SystemConfigGeneral() = delete;
    SystemConfigGeneral(const SystemConfigGeneral &) = delete;
    SystemConfigGeneral & operator=(const SystemConfigGeneral &) = delete;

    ISciterUI & m_sciterUI;
    SystemConfig & m_config;
    ISciterWindow & m_window;
    SciterElement m_page;
    std::shared_ptr<IPageNav> m_pageNav;
    std::unique_ptr<SystemConfigLogging> m_systemConfigLogging;
    std::unique_ptr<SystemConfigHotkeys> m_systemConfigHotkeys;
    std::unique_ptr<SystemConfigSystem> m_systemConfigSystem;
};