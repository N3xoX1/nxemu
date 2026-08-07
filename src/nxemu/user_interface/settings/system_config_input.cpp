#include "system_config_input.h"
#include "config_setting.h"
#include "system_config.h"
#include <nxemu-os/os_settings_identifiers.h>

namespace
{
static ConfigSetting inputSettings[] = {
    ConfigSetting(ConfigSetting::CheckBox, "VibrationEnabled", true, NXOsSetting::VibrationEnabled),
    ConfigSetting(ConfigSetting::CheckBox, "EnableAccurateVibrations", true, NXOsSetting::EnableAccurateVibrations),
};
}

SystemConfigInput::SystemConfigInput(ISciterUI & sciterUI, SystemConfig & config, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_page(page)
{
    m_config.SetupPage(page, inputSettings, sizeof(inputSettings) / sizeof(inputSettings[0]));
}

void SystemConfigInput::SaveSetting(void)
{
    m_config.SavePage(m_page, inputSettings, sizeof(inputSettings) / sizeof(inputSettings[0]));
}
