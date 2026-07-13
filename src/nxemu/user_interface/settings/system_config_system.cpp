#include "system_config_system.h"
#include "config_setting.h"
#include "settings/ui_identifiers.h"
#include "system_config.h"
#include <common/std_string.h>
#include <nxemu-loader/loader_settings_identifiers.h>
#include <nxemu-os/os_settings_identifiers.h>

namespace
{
static ConfigSetting systemSettings[] = {
    ConfigSetting(ConfigSetting::ComboBox, "DockedMode", true, SystemConfig::TranslationType::DockedMode, NXOsSetting::DockedMode),
    ConfigSetting(ConfigSetting::Slider, "SpeedLimit", true, NXOsSetting::SpeedLimit),
    ConfigSetting(ConfigSetting::CheckBox, "CheckForUpdatedFirmware", true, NXLoaderSetting::CheckForUpdatedFirmware),
    ConfigSetting(ConfigSetting::CheckBox, "ConfirmBeforeStopping", true, NXUISetting::ConfirmBeforeStopping),
    ConfigSetting(ConfigSetting::CheckBox, "HideMouseOnInactivity", true, NXUISetting::HideMouseOnInactivity),
    ConfigSetting(ConfigSetting::CheckBox, "EnableDiscordPresence", true, NXUISetting::EnableDiscordPresence),
};
}

SystemConfigSystem::SystemConfigSystem(ISciterUI & sciterUI, SystemConfig & config, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_page(page)
{
    m_config.SetupPage(page, systemSettings, sizeof(systemSettings) / sizeof(systemSettings[0]));
    m_sciterUI.AttachHandler(page.GetElementByID("SpeedLimit"), IID_ISTATECHANGESINK, (IStateChangeSink *)this);
    UpdateSpeedLimitDisplay();
}

void SystemConfigSystem::SaveSetting(void)
{
    m_config.SavePage(m_page, systemSettings, sizeof(systemSettings) / sizeof(systemSettings[0]));
}

bool SystemConfigSystem::OnStateChange(SCITER_ELEMENT elem, uint32_t /*eventReason*/, void * /*data*/)
{
    if (m_page.GetElementByID("SpeedLimit") == elem)
    {
        UpdateSpeedLimitDisplay();
    }
    return false;
}

void SystemConfigSystem::UpdateSpeedLimitDisplay()
{
    SciterElement speedLimit = m_page.GetElementByID("SpeedLimit");
    SciterElement speedLimitDisplay = m_page.GetElementByID("SpeedLimitDisplay");

    if (speedLimit && speedLimitDisplay)
    {
        SciterValue value = speedLimit.GetValue();
        if (value.isInt())
        {
            stdstr_f text("%d %%", value.GetValueInt());
            speedLimitDisplay.SetHTML((const uint8_t *)text.c_str(), text.size());
        }
    }
}
