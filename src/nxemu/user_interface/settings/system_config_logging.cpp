#include "config_setting.h"
#include "system_config.h"
#include "system_config_logging.h"
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-os/os_settings_identifiers.h>

namespace
{
    static ConfigSetting loggingSettings[] = {
        ConfigSetting(ConfigSetting::CheckBox, "ShowLogConsole", true, NXCoreSetting::ShowLogConsole),
        ConfigSetting(ConfigSetting::CheckBox, "UseMultiCore", false, NXOsSetting::UseMultiCore),
        ConfigSetting(ConfigSetting::InputText, "LogFilter", true, NXCoreSetting::LogFilter),
    };
}

SystemConfigLogging::SystemConfigLogging(ISciterUI & sciterUI, SystemConfig & config, ISciterWindow & window, SciterElement page) :
    m_sciterUI(sciterUI),
    m_config(config),
    m_window(window),
    m_page(page)
{
    m_config.SetupPage(m_page, loggingSettings, sizeof(loggingSettings) / sizeof(loggingSettings[0]));
}

void SystemConfigLogging::SaveSetting(void)
{
    m_config.SavePage(m_page, loggingSettings, sizeof(loggingSettings) / sizeof(loggingSettings[0]));
}