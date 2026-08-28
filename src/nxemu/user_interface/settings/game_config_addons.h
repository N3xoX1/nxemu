#pragma once
#include <nxemu-module-spec/base.h>
#include <sciter_element.h>
#include <sciter_handler.h>
#include <sciter_ui.h>
#include <string>
#include <vector>

class GameConfig;
class SystemModules;
nxinterface IRomInfo;

class GameConfigAddons :
    public IClickSink
{
public:
    GameConfigAddons(ISciterUI & sciterUI, GameConfig & config, SystemModules & modules, IRomInfo & romInfo, SciterElement page);
    ~GameConfigAddons() = default;

    void SaveSetting(void);
    void RevertSetting(void);

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

private:
    GameConfigAddons() = delete;
    GameConfigAddons(const GameConfigAddons &) = delete;
    GameConfigAddons & operator=(const GameConfigAddons &) = delete;

    void PopulateAddons(void);
    void ApplyDisabledAddons(void);
    void RestoreOriginalDisabledAddons(void);
    void EnforceSingleEnabledUpdate(SciterElement clickedCheckbox);

    ISciterUI & m_sciterUI;
    GameConfig & m_config;
    SystemModules & m_modules;
    IRomInfo & m_romInfo;
    SciterElement m_page;
    std::vector<std::string> m_originalDisabled;
};
