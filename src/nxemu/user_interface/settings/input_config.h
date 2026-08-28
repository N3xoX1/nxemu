#pragma once
#include <sciter_handler.h>
#include <sciter_ui.h>
#include <widgets/page_nav.h>
#include <memory>
#include <unordered_map>
#include <nxemu-module-spec/base.h>

nxinterface IParamPackageList;
class InputConfigPlayer;
class SystemModules;

enum class NpadIdType : uint32_t;

class InputConfig :
    public IPagesSink,
    public IClickSink,
    public IKeySink,
    public IWindowDestroySink,
    public IWindowCloseSink
{
    typedef std::unordered_map<std::string, std::pair<size_t, NpadIdType>> PlayerInfo;

public:
    InputConfig(ISciterUI & SciterUI, SystemModules & modules);
    ~InputConfig();

    void Display(void * parentWindow);

    const IParamPackageList & InputDeviceList() const;

    // IPagesSink
    bool PageNavChangeFrom(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    bool PageNavChangeTo(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    void PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page) override;
    void PageNavPageChanged(const std::string & pageName, SCITER_ELEMENT pageNav) override;

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

    // IKeySink
    bool OnKeyDown(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState) override;
    bool OnKeyUp(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState) override;
    bool OnKeyChar(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState) override;

    // IWindowDestroySink
    void OnWindowDestroy(HWINDOW hWnd) override;

    // IWindowCloseSink
    bool OnWindowCloseRequest(HWINDOW hWnd) override;

private:
    InputConfig() = delete;
    InputConfig(const InputConfig &) = delete;
    InputConfig & operator=(const InputConfig &) = delete;

    void ShutdownPlayers();

    ISciterUI & m_sciterUI;
    SystemModules & m_modules;
    ISciterWindow * m_window;
    std::shared_ptr<IPageNav> m_pageNav;
    std::unique_ptr<InputConfigPlayer> m_playerConfig[8];
    InputConfigPlayer * m_playerCurrent;
    IParamPackageList * m_inputDeviceList;
    const PlayerInfo m_playerMap;
};
