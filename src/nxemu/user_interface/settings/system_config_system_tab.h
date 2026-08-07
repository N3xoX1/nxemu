#pragma once
#include <memory>
#include <sciter_element.h>
#include <sciter_ui.h>
#include <widgets/page_nav.h>

class SystemConfig;
class SystemConfigInput;

class SystemConfigSystemTab :
    public IPagesSink
{
public:
    SystemConfigSystemTab(ISciterUI & sciterUI, SystemConfig & config, SciterElement page);
    ~SystemConfigSystemTab();

    void SaveSetting(void);
    void SetInitialPage(const char * path);

    // IPagesSink
    bool PageNavChangeFrom(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    bool PageNavChangeTo(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    void PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page) override;
    void PageNavPageChanged(const std::string & pageName, SCITER_ELEMENT pageNav) override;

private:
    SystemConfigSystemTab() = delete;
    SystemConfigSystemTab(const SystemConfigSystemTab &) = delete;
    SystemConfigSystemTab & operator=(const SystemConfigSystemTab &) = delete;

    ISciterUI & m_sciterUI;
    SystemConfig & m_config;
    SciterElement m_page;
    std::shared_ptr<IPageNav> m_pageNav;
    std::unique_ptr<SystemConfigInput> m_systemConfigInput;
};
