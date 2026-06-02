#include "config_setting.h"
#include "settings/ui_identifiers.h"
#include "settings/ui_settings.h"
#include "system_config.h"
#include "system_config_game_browser.h"
#include <common/path.h>
#include <nxemu-core/settings/identifiers.h>
#include <widgets/list_box.h>

namespace
{
static ConfigSetting gameBrowserSettings[] = {
    ConfigSetting(ConfigSetting::Slider, "myGamesIconSize", true, NXUISetting::MyGameIconSize),
    ConfigSetting(ConfigSetting::CheckBox, "ConfirmBeforeStopping", true, NXUISetting::ConfirmBeforeStopping),
    ConfigSetting(ConfigSetting::ListBox, "gameDirectoryList", true, NXUISetting::GameDirectories),
};
}

SystemConfigGameBrowser::SystemConfigGameBrowser(ISciterUI & sciterUI, SystemConfig & config, ISciterWindow & window, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_window(window),
    m_page(page)
{
    m_config.SetupPage(page, gameBrowserSettings, sizeof(gameBrowserSettings) / sizeof(gameBrowserSettings[0]));

    SciterElement root(m_window.GetRootElement());
    if (root.IsValid())
    {
        SciterElement addButton = root.GetElementByID("gameDirectoryAdd");
        m_sciterUI.AttachHandler(addButton, IID_ICLICKSINK, (IClickSink *)this);
        SciterElement removeButton = root.GetElementByID("gameDirectoryRemove");
        m_sciterUI.AttachHandler(removeButton, IID_ICLICKSINK, (IClickSink *)this);
    }
}

bool SystemConfigGameBrowser::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/, uint32_t /*reason*/)
{
    SciterElement clickElem(element);
    std::string elementID = clickElem.GetAttributeByName("id");
    if (elementID == "gameDirectoryAdd")
    {
        Path newDirectory = Path().BrowseForDirectory((void*)m_window.GetHandle(), "Select Game Directory");
        if (newDirectory.DirectoryExists())
        {
            std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(m_page.GetElementByID("gameDirectoryList"), IID_ILISTBOX);
            if (interfacePtr)
            {
                std::shared_ptr<IListBox> listBox = std::static_pointer_cast<IListBox>(interfacePtr);
                listBox->AddItem(newDirectory, newDirectory);
            }
        }
    }
    else if (elementID == "gameDirectoryRemove")
    {
        RemoveSelectedDirectory();
    }
    return true;
}

void SystemConfigGameBrowser::SelectDirectoryItem(const SciterElement & gameDirectoryList, SCITER_ELEMENT source)
{
    SciterElement target = source, directoryItem;
    while (target.IsValid() && target != gameDirectoryList)
    {
        std::string className = target.GetAttribute("class");
        if (!className.empty())
        {
            std::string searchIn = " " + className + " ";
            if (searchIn.find(" directory-item ") != std::string::npos)
            {
                directoryItem = target;
                break;
            }
        }
        target = target.GetParent();
    }

    if (target.IsValid())
    {
        for (uint32_t i = 0, n = gameDirectoryList.GetChildCount(); i < n; i++)
        {
            SciterElement(gameDirectoryList.GetChild(i)).SetState(0, SciterElement::STATE_CHECKED, true);
        }
        target.SetState(SciterElement::STATE_CHECKED, 0, true);
    }
}

void SystemConfigGameBrowser::RemoveSelectedDirectory()
{
    SciterElement gameDirectoryList = m_page.GetElementByID("gameDirectoryList");
    if (!gameDirectoryList.IsValid())
    {
        return;
    }
    std::shared_ptr<void> interfacePtr = m_sciterUI.GetElementInterface(m_page.GetElementByID("gameDirectoryList"), IID_ILISTBOX);
    if (!interfacePtr)
    {
        return;
    }
    std::shared_ptr<IListBox> listBox = std::static_pointer_cast<IListBox>(interfacePtr);
    listBox->RemoveItem(listBox->CurrentIndex());
}

void SystemConfigGameBrowser::SaveSetting(void)
{
    m_config.SavePage(m_page, gameBrowserSettings, sizeof(gameBrowserSettings) / sizeof(gameBrowserSettings[0]));
}