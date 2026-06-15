#pragma once
#include <sciter_handler.h>
#include <widgets/page_nav.h>
#include <map>
#include <vector>
#include "startup_checks.h"

__interface ISciterUI;
__interface ISciterWindow;
class SciterElement;
class SystemConfigAudio;
class SystemConfigGraphics;
class SystemConfigGeneral;
class SystemConfigSystem;
class ConfigSetting;
class SystemModules;

typedef std::pair<int32_t, std::string> SettingTranslation;
typedef std::vector<SettingTranslation> SettingTranslationList;
typedef std::map<uint32_t, SettingTranslationList> SettingTranslationMap;

class SystemConfig :
    public IPagesSink,
    public IClickSink
{
public:
    SystemConfig(ISciterUI & SciterUI, SystemModules & modules, std::vector<VkDeviceRecord> & vkDeviceRecords);
    ~SystemConfig();

    void Display(void * parentWindow, const char * startPage);
    void SavePage(SCITER_ELEMENT pageElement, const ConfigSetting * settings, size_t settingsCount);
    void SetupPage(SCITER_ELEMENT pageElement, const ConfigSetting * settings, size_t settingsCount);    

    // IPagesSink
    bool PageNavChangeFrom(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    bool PageNavChangeTo(const std::string & pageName, SCITER_ELEMENT pageNav) override;
    void PageNavCreatedPage(const std::string & pageName, SCITER_ELEMENT page) override;
    void PageNavPageChanged(const std::string & pageName, SCITER_ELEMENT pageNav) override;

    //IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

private:
    SystemConfig() = delete;
    SystemConfig(const SystemConfig &) = delete;
    SystemConfig & operator=(const SystemConfig &) = delete;

    void InitializeTranslations();
    void SaveComboBox(const SciterElement & page, const ConfigSetting & setting, bool intValue);
    void SetupComboBox(const SciterElement & page, const ConfigSetting & setting);

    ISciterUI & m_sciterUI;
    SystemModules & m_modules;
    std::vector<VkDeviceRecord> & m_vkDeviceRecords;
    ISciterWindow * m_window;
    SettingTranslationMap m_settingTranslations;
    std::shared_ptr<IPageNav> m_pageNav;
    std::unique_ptr<SystemConfigGraphics> m_systemConfigGraphics;
    std::unique_ptr<SystemConfigAudio> m_systemConfigAudio;
    std::unique_ptr<SystemConfigGeneral> m_systemConfigGeneral;
};
