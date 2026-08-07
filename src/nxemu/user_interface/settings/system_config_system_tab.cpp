#include "system_config_system_tab.h"
#include "system_config_input.h"

SystemConfigSystemTab::SystemConfigSystemTab(ISciterUI & sciterUI, SystemConfig & config, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_page(page)
{
    SciterElement pageNav = page.GetElementByID("SystemTabNav");
    std::shared_ptr<void> interfacePtr = pageNav.IsValid() ? m_sciterUI.GetElementInterface(pageNav, IID_IPAGENAV) : nullptr;
    if (interfacePtr)
    {
        m_pageNav = std::static_pointer_cast<IPageNav>(interfacePtr);
        m_pageNav->AddSink(this);
    }
}

SystemConfigSystemTab::~SystemConfigSystemTab()
{
}

void SystemConfigSystemTab::SetInitialPage(const char * path)
{
    if (path == nullptr || path[0] == '\0' || !m_pageNav)
    {
        return;
    }
    m_pageNav->SetCurrentPage(path);
}

void SystemConfigSystemTab::SaveSetting(void)
{
    if (m_systemConfigInput)
    {
        m_systemConfigInput->SaveSetting();
    }
}

bool SystemConfigSystemTab::PageNavChangeFrom(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

bool SystemConfigSystemTab::PageNavChangeTo(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

void SystemConfigSystemTab::PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page)
{
    if (pageName == "Input")
    {
        m_systemConfigInput.reset(new SystemConfigInput(m_sciterUI, m_config, page));
    }
}

void SystemConfigSystemTab::PageNavPageChanged(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
}
