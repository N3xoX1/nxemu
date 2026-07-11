#pragma once
#include <sciter_ui.h>
#include <sciter_element.h>
#include <sciter_handler.h>

class SystemConfig;

class SystemConfigSystem :
    public IStateChangeSink
{
public:
    SystemConfigSystem(ISciterUI & sciterUI, SystemConfig & config, SciterElement page);
    ~SystemConfigSystem() = default;

    void SaveSetting(void);

    // IStateChangeSink
    bool OnStateChange(SCITER_ELEMENT elem, uint32_t eventReason, void * data) override;

private:
    SystemConfigSystem() = delete;
    SystemConfigSystem(const SystemConfigSystem &) = delete;
    SystemConfigSystem & operator=(const SystemConfigSystem &) = delete;

    void UpdateSpeedLimitDisplay();

    ISciterUI & m_sciterUI;
    SystemConfig & m_config;
    SciterElement m_page;
};
