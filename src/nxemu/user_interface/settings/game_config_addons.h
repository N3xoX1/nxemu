#pragma once
#include <sciter_element.h>

class GameConfig;
class SystemModules;
__interface ISciterUI;

class GameConfigAddons
{
public:
    GameConfigAddons(ISciterUI & sciterUI, GameConfig & config, SystemModules & modules, SciterElement page);
    ~GameConfigAddons() = default;

    void SaveSetting(void);

private:
    GameConfigAddons() = delete;
    GameConfigAddons(const GameConfigAddons &) = delete;
    GameConfigAddons & operator=(const GameConfigAddons &) = delete;

    void PopulateAddons(void);

    ISciterUI & m_sciterUI;
    GameConfig & m_config;
    SystemModules & m_modules;
    SciterElement m_page;
};
