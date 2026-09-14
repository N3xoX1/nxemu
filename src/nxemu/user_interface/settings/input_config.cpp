#include "input_config.h"
#include "input_config_player.h"
#include <nxemu-core/modules/system_modules.h>
#include <sciter_ui.h>

InputConfig::InputConfig(ISciterUI & SciterUI, SystemModules & modules) :
    m_sciterUI(SciterUI),
    m_modules(modules),
    m_window(nullptr),
    m_inputDeviceList(nullptr),
    m_playerCurrent(nullptr),
    m_playerMap({ 
        {"Player1", {0, NpadIdType::Player1}},
        {"Player2", {1, NpadIdType::Player2}},
        {"Player3", {2, NpadIdType::Player3}},
        {"Player4", {3, NpadIdType::Player4}},
        {"Player5", {4, NpadIdType::Player5}},
        {"Player6", {5, NpadIdType::Player6}},
        {"Player7", {6, NpadIdType::Player7}},
        {"Player8", {7, NpadIdType::Player8}}, 
    })
{
}

InputConfig::~InputConfig()
{
    ShutdownPlayers();
    if (m_inputDeviceList != nullptr)
    {
        m_inputDeviceList->Release();
        m_inputDeviceList = nullptr;
    }
}

void InputConfig::Display(void * parentWindow)
{
    if (!m_modules.IsValid() || !m_sciterUI.WindowCreate(parentWindow, "input_config.html", 0, 0, 0, 0, SUIW_CHILD | SUIW_HIDDEN, m_window))
    {
        return;
    }
    m_window->OnDestroySinkAdd((IWindowDestroySink *)this);
    m_window->OnCloseSinkAdd((IWindowCloseSink *)this);

    IOperatingSystem & operatingSystem = m_modules.Modules().OperatingSystem();
    m_inputDeviceList = operatingSystem.GetInputDevices();

    SciterElement root(m_window->GetRootElement());
    if (root.IsValid())
    {
        SciterElement pageNav = root.GetElementByID("MainTabNav");
        std::shared_ptr<void> interfacePtr = pageNav.IsValid() ? m_sciterUI.GetElementInterface(pageNav, IID_IPAGENAV) : nullptr;
        if (interfacePtr)
        {
            m_pageNav = std::static_pointer_cast<IPageNav>(interfacePtr);
            m_pageNav->AddSink(this);
        }

        SciterElement okButton = root.FindFirst("button[role=\"window-ok\"]");
        m_sciterUI.AttachHandler(okButton, IID_ICLICKSINK, (IClickSink*)this);

        m_sciterUI.AttachHandler(root, IID_IKEYSINK, (IKeySink*)this);
    }

    m_window->FixMinSize();
    m_window->CenterWindow();
    m_window->Show();
}

const IParamPackageList & InputConfig::InputDeviceList() const
{
    return *m_inputDeviceList;
}

bool InputConfig::PageNavChangeFrom(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

bool InputConfig::PageNavChangeTo(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

void InputConfig::PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page)
{
    PlayerInfo::const_iterator itr = m_playerMap.find(pageName);
    if (itr != m_playerMap.end())
    {
        m_playerConfig[itr->second.first].reset(new InputConfigPlayer(m_sciterUI, *this, m_modules, m_window->GetHandle(), page, itr->second.second));
    }
}

void InputConfig::PageNavPageChanged(const std::string & pageName, SCITER_ELEMENT /*pageNav*/)
{
    PlayerInfo::const_iterator itr = m_playerMap.find(pageName);
    if (itr != m_playerMap.end())
    {
        m_playerCurrent = m_playerConfig[itr->second.first].get();
    }
}

bool InputConfig::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/, uint32_t /*reason*/)
{
    SciterElement clickElem(element);
    if (clickElem.GetAttribute("role") == "window-ok")
    {
        for (size_t i = 0, n = sizeof(m_playerConfig) / sizeof(m_playerConfig[0]); i < n; i++)
        {
            if (m_playerConfig[i] == nullptr)
            {
                continue;
            }
            m_playerConfig[i]->SaveSetting();
        }
        m_modules.FlushSettings();
        m_window->Destroy();
    }
    return false;
}

bool InputConfig::OnKeyDown(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState)
{
    return m_playerCurrent != nullptr ? m_playerCurrent->OnKeyDown(element, item, keyCode, keyboardState) : false;
}

bool InputConfig::OnKeyUp(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState)
{
    return m_playerCurrent != nullptr ? m_playerCurrent->OnKeyUp(element, item, keyCode, keyboardState) : false;
}

bool InputConfig::OnKeyChar(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState)
{
    return m_playerCurrent != nullptr ? m_playerCurrent->OnKeyChar(element, item, keyCode, keyboardState) : false;
}

void InputConfig::ShutdownPlayers()
{
    for (size_t i = 0, n = sizeof(m_playerConfig) / sizeof(m_playerConfig[0]); i < n; i++)
    {
        m_playerConfig[i].reset();
    }
    m_playerCurrent = nullptr;
}

bool InputConfig::OnWindowCloseRequest(HWINDOW hWnd)
{
    if (m_window != nullptr && hWnd == m_window->GetHandle())
    {
        ShutdownPlayers();
    }
    return true;
}

void InputConfig::OnWindowDestroy(HWINDOW hWnd)
{
    if (m_window == nullptr || hWnd != m_window->GetHandle())
    {
        return;
    }
    ShutdownPlayers();
    if (m_inputDeviceList != nullptr)
    {
        m_inputDeviceList->Release();
        m_inputDeviceList = nullptr;
    }
    m_window = nullptr;
}
