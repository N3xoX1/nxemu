#pragma once

#include <sciter_handler.h>
#include <sciter_ui.h>
#include <widgets/page_nav.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <string>
#include <vector>

class SystemModules;
class GameConfigAddons;

class GameConfig :
    public IClickSink,
    public IPagesSink,
    public IWindowDestroySink
{
public:
    GameConfig(ISciterUI & sciterUI, SystemModules & modules);
    ~GameConfig();

    void Display(void * parentWindow, const char * gamePath);
    uint64_t ProgramId() const { return m_programId; }
    const std::string & GamePath() const { return m_gamePath; }
    IRomInfo * RomInfo() const { return m_romInfo.Get(); }
    void RefreshControlMetadata();

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

    // IPagesSink
    bool PageNavChangeFrom(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    bool PageNavChangeTo(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    void PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page) override;
    void PageNavPageChanged(const std::string & pageName, SCITER_ELEMENT pageNav) override;

    // IWindowDestroySink
    void OnWindowDestroy(HWINDOW hWnd) override;

private:
    GameConfig() = delete;
    GameConfig(const GameConfig &) = delete;
    GameConfig & operator=(const GameConfig &) = delete;

    void PopulateInfo();

    ISciterUI & m_sciterUI;
    SystemModules & m_modules;
    ISciterWindow * m_window;
    std::shared_ptr<IPageNav> m_pageNav;
    std::unique_ptr<GameConfigAddons> m_gameConfigAddons;
    IRomInfoPtr m_romInfo;
    uint64_t m_programId;
    bool m_revertAddonsOnClose;
    std::string m_gamePath;
    std::string m_title;
    std::string m_developer;
    std::string m_version;
    std::string m_format;
    std::string m_size;
    std::string m_filename;
    std::vector<uint8_t> m_icon;
};
