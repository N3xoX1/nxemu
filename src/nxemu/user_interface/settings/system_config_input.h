#pragma once
#include <sciter_element.h>
#include <sciter_ui.h>

class SystemConfig;

class SystemConfigInput
{
public:
    SystemConfigInput(ISciterUI & sciterUI, SystemConfig & config, SciterElement page);
    ~SystemConfigInput() = default;

    void SaveSetting(void);

private:
    SystemConfigInput() = delete;
    SystemConfigInput(const SystemConfigInput &) = delete;
    SystemConfigInput & operator=(const SystemConfigInput &) = delete;

    ISciterUI & m_sciterUI;
    SystemConfig & m_config;
    SciterElement m_page;
};
