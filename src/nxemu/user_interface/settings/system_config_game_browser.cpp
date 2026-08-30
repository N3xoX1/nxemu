#include "config_setting.h"
#include "settings/ui_identifiers.h"
#include "system_config.h"
#include "system_config_game_browser.h"
#include "user_interface/file_dialogs.h"
#include <common/path.h>
#include <nxemu-loader/loader_settings_identifiers.h>
#include <widgets/list_box.h>

namespace
{
static ConfigSetting gamesSettings[] = {
    ConfigSetting(ConfigSetting::Slider, "myGamesIconSize", true, NXUISetting::MyGameIconSize),
    ConfigSetting(ConfigSetting::ListBox, "gameDirectoryList", true, NXUISetting::GameDirectories),
};

static ConfigSetting addOnsSettings[] = {
    ConfigSetting(ConfigSetting::ListBox, "addOnDirectoryList", true, NXLoaderSetting::AddOnDirectories),
};
}

SystemConfigGameBrowser::SystemConfigGameBrowser(ISciterUI & sciterUI, SystemConfig & config, ISciterWindow & window, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_window(window),
    m_page(page)
{
    SciterElement pageNav = page.GetElementByID("GameBrowserTabNav");
    std::shared_ptr<void> interfacePtr = pageNav.IsValid() ? m_sciterUI.GetElementInterface(pageNav, IID_IPAGENAV) : nullptr;
    if (interfacePtr)
    {
        m_pageNav = std::static_pointer_cast<IPageNav>(interfacePtr);
        m_pageNav->AddSink(this);
    }
}

void SystemConfigGameBrowser::SetInitialPage(const char * path)
{
    if (path == nullptr || path[0] == '\0' || !m_pageNav)
    {
        return;
    }
    m_pageNav->SetCurrentPage(path);
}

void SystemConfigGameBrowser::SetupGamesPage(SciterElement page)
{
    m_gamesPage = page;
    m_config.SetupPage(page, gamesSettings, sizeof(gamesSettings) / sizeof(gamesSettings[0]));
    m_sciterUI.AttachHandler(page.GetElementByID("gameDirectoryAdd"), IID_ICLICKSINK, (IClickSink *)this);
    m_sciterUI.AttachHandler(page.GetElementByID("gameDirectoryRemove"), IID_ICLICKSINK, (IClickSink *)this);
}

void SystemConfigGameBrowser::SetupAddOnsPage(SciterElement page)
{
    m_addOnsPage = page;
    m_config.SetupPage(page, addOnsSettings, sizeof(addOnsSettings) / sizeof(addOnsSettings[0]));
    m_sciterUI.AttachHandler(page.GetElementByID("addOnDirectoryAdd"), IID_ICLICKSINK, (IClickSink *)this);
    m_sciterUI.AttachHandler(page.GetElementByID("addOnDirectoryRemove"), IID_ICLICKSINK, (IClickSink *)this);
}

bool SystemConfigGameBrowser::PageNavChangeFrom(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

bool SystemConfigGameBrowser::PageNavChangeTo(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
    return true;
}

void SystemConfigGameBrowser::PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page)
{
    if (pageName == "Games")
    {
        SetupGamesPage(page);
    }
    else if (pageName == "AddOns")
    {
        SetupAddOnsPage(page);
    }
}

void SystemConfigGameBrowser::PageNavPageChanged(const std::string & /*pageName*/, SCITER_ELEMENT /*pageNav*/)
{
}

bool SystemConfigGameBrowser::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/, uint32_t /*reason*/)
{
    SciterElement clickElem(element);
    const std::string elementID = clickElem.GetAttributeByName("id");
    if (elementID == "gameDirectoryAdd")
    {
        Path newDirectory = BrowseForDirectory((void *)m_window.GetHandle(), "Select Game Directory");
        if (newDirectory.DirectoryExists() && m_gamesPage.IsValid())
        {
            std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(m_gamesPage.GetElementByID("gameDirectoryList"), IID_ILISTBOX);
            if (interfacePtr)
            {
                std::shared_ptr<IListBox> listBox = std::static_pointer_cast<IListBox>(interfacePtr);
                listBox->AddItem(newDirectory, newDirectory);
            }
        }
    }
    else if (elementID == "gameDirectoryRemove")
    {
        RemoveSelectedDirectory(m_gamesPage, "gameDirectoryList");
    }
    else if (elementID == "addOnDirectoryAdd")
    {
        Path newDirectory = BrowseForDirectory((void *)m_window.GetHandle(), "Select Add-On Directory");
        if (newDirectory.DirectoryExists() && m_addOnsPage.IsValid())
        {
            std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(m_addOnsPage.GetElementByID("addOnDirectoryList"), IID_ILISTBOX);
            if (interfacePtr)
            {
                std::shared_ptr<IListBox> listBox = std::static_pointer_cast<IListBox>(interfacePtr);
                listBox->AddItem(newDirectory, newDirectory);
            }
        }
    }
    else if (elementID == "addOnDirectoryRemove")
    {
        RemoveSelectedDirectory(m_addOnsPage, "addOnDirectoryList");
    }
    return true;
}

void SystemConfigGameBrowser::RemoveSelectedDirectory(const SciterElement & page, const char * listId)
{
    if (!page.IsValid())
    {
        return;
    }
    SciterElement directoryList = page.GetElementByID(listId);
    if (!directoryList.IsValid())
    {
        return;
    }
    std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(directoryList, IID_ILISTBOX);
    if (!interfacePtr)
    {
        return;
    }
    std::shared_ptr<IListBox> listBox = std::static_pointer_cast<IListBox>(interfacePtr);
    listBox->RemoveItem(listBox->CurrentIndex());
}

void SystemConfigGameBrowser::SaveSetting(void)
{
    if (m_gamesPage.IsValid())
    {
        m_config.SavePage(m_gamesPage, gamesSettings, sizeof(gamesSettings) / sizeof(gamesSettings[0]));
    }
    if (m_addOnsPage.IsValid())
    {
        m_config.SavePage(m_addOnsPage, addOnsSettings, sizeof(addOnsSettings) / sizeof(addOnsSettings[0]));
    }
}