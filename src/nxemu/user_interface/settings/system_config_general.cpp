#include "system_config_general.h"
#include "system_config_hotkeys.h"
#include "system_config_logging.h"
#include "system_config_system.h"

SystemConfigGeneral::SystemConfigGeneral(ISciterUI & sciterUI, SystemConfig & config, ISciterWindow & window, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_window(window),
    m_page(page)
{
    SciterElement pageNav = page.GetElementByID("GeneralTabNav");
    std::shared_ptr<void> interfacePtr = pageNav.IsValid() ? m_sciterUI.GetElementInterface(pageNav, IID_IPAGENAV) : nullptr;
    if (interfacePtr)
    {
        m_pageNav = std::static_pointer_cast<IPageNav>(interfacePtr);
        m_pageNav->AddSink(this);
    }
}

SystemConfigGeneral::~SystemConfigGeneral()
{
}

void SystemConfigGeneral::SetInitialPage(const char * path)
{
    if (path == nullptr || path[0] == '\0' || !m_pageNav)
    {
        return;
    }
    m_pageNav->SetCurrentPage(path);
}

void SystemConfigGeneral::SaveSetting(void)
{
    if (m_systemConfigLogging)
    {
        m_systemConfigLogging->SaveSetting();
    }
    if (m_systemConfigHotkeys)
    {
        m_systemConfigHotkeys->SaveSetting();
    }
    if (m_systemConfigSystem)
    {
        m_systemConfigSystem->SaveSetting();
    }
}

bool SystemConfigGeneral::PageNavChangeFrom(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

bool SystemConfigGeneral::PageNavChangeTo(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

void SystemConfigGeneral::PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page)
{
    if (pageName == "Logging")
    {
        m_systemConfigLogging.reset(new SystemConfigLogging(m_sciterUI, m_config, m_window, page));
    }
    else if (pageName == "Hotkeys")
    {
        m_systemConfigHotkeys.reset(new SystemConfigHotkeys(m_sciterUI, m_window, page));
    }
    else if (pageName == "General")
    {
        m_systemConfigSystem.reset(new SystemConfigSystem(m_sciterUI, m_config, page));
    }
}

void SystemConfigGeneral::PageNavPageChanged(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
}
