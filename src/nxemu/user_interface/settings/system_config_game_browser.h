#pragma once
#include <sciter_ui.h>
#include <sciter_element.h>
#include <sciter_handler.h>
#include <widgets/page_nav.h>

class SystemConfig;

class SystemConfigGameBrowser :
    public IPagesSink,
    public IClickSink
{
public:
    SystemConfigGameBrowser(ISciterUI & sciterUI, SystemConfig & config, ISciterWindow & window, SciterElement page);
    ~SystemConfigGameBrowser() = default;

    void SaveSetting(void);
    void SetInitialPage(const char * path);

    // IPagesSink
    bool PageNavChangeFrom(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    bool PageNavChangeTo(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    void PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page) override;
    void PageNavPageChanged(const std::string & pageName, SCITER_ELEMENT pageNav) override;

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

private:
    SystemConfigGameBrowser() = delete;
    SystemConfigGameBrowser(const SystemConfigGameBrowser &) = delete;
    SystemConfigGameBrowser & operator=(const SystemConfigGameBrowser &) = delete;

    void SetupGamesPage(SciterElement page);
    void SetupAddOnsPage(SciterElement page);
    void RemoveSelectedDirectory(const SciterElement & page, const char * listId);

    ISciterUI & m_sciterUI;
    SystemConfig & m_config;
    ISciterWindow & m_window;
    SciterElement m_page;
    SciterElement m_gamesPage;
    SciterElement m_addOnsPage;
    std::shared_ptr<IPageNav> m_pageNav;
};