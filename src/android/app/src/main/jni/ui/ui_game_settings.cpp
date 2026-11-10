#include "ui_game_settings.h"
#include "emulation_session.h"

#include <cstdio>
#include <cstdlib>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <string>
#include <string_view>

namespace
{
    struct GameSetting
    {
        const char * system;
        const char * game;
        bool isBool;
    };

    constexpr GameSetting kGameSettings[] = {
        {NXOsSetting::SystemDockedMode, NXOsSetting::GameDockedMode, false},
        {NXCoreSetting::SystemLanguageIndex, NXOsSetting::GameLanguageIndex, false},
        {NXOsSetting::SystemSpeedLimit, NXOsSetting::GameSpeedLimit, false},
        {NXVideoSetting::SystemUseDiskPipelineCache, NXVideoSetting::GameUseDiskPipelineCache, true},
        {NXVideoSetting::SystemEnableReactiveFlushing, NXVideoSetting::GameEnableReactiveFlushing, true},
        {NXVideoSetting::SystemAccuracyLevel, NXVideoSetting::GameAccuracyLevel, false},
        {NXVideoSetting::SystemAnisotropicFiltering, NXVideoSetting::GameAnisotropicFiltering, false},
        {NXVideoSetting::SystemVSyncMode, NXVideoSetting::GameVSyncMode, false},
        {NXVideoSetting::SystemAspectRatio, NXVideoSetting::GameAspectRatio, false},
        {NXVideoSetting::SystemResolutionSetup, NXVideoSetting::GameResolutionSetup, false},
        {NXVideoSetting::SystemScalingFilter, NXVideoSetting::GameScalingFilter, false},
        {NXVideoSetting::SystemAntiAliasing, NXVideoSetting::GameAntiAliasing, false},
        {NXVideoSetting::SystemFSPSharpness, NXVideoSetting::GameFSPSharpness, false},
        {NXVideoSetting::SystemForceMaximumClocks, NXVideoSetting::GameForceMaximumClocks, true},
        {NXVideoSetting::SystemUseAsynchronousShaderBuilding, NXVideoSetting::GameUseAsynchronousShaderBuilding, true},
    };

    uint64_t s_programId = 0;
    bool s_titleIdSet = false;

    SettingsStore & Store()
    {
        return SettingsStore::GetInstance();
    }

    const GameSetting * Find(const char * key)
    {
        for (const GameSetting & setting : kGameSettings)
        {
            if (std::string_view(setting.system) == key)
            {
                return &setting;
            }
        }
        return nullptr;
    }

    bool IsOverridden(const GameSetting & setting)
    {
        return setting.isBool ? Store().GetStoredBool(setting.game) != Store().GetStoredBool(setting.system)
                              : Store().GetStoredInt(setting.game) != Store().GetStoredInt(setting.system);
    }

    bool GameRunning()
    {
        return Store().GetBool(NXCoreSetting::EmulationRunning);
    }

    const char * ReadKey(const char * key)
    {
        const GameSetting * setting = Find(key);
        return setting != nullptr && (s_programId != 0 || GameRunning()) ? setting->game : key;
    }

    const char * WriteKey(const char * key)
    {
        const GameSetting * setting = Find(key);
        if (setting == nullptr)
        {
            return key;
        }
        return s_programId != 0 || (GameRunning() && IsOverridden(*setting)) ? setting->game : key;
    }

    std::string TitleId(uint64_t programId)
    {
        char titleId[17];
        std::snprintf(titleId, sizeof(titleId), "%016llX", static_cast<unsigned long long>(programId));
        return titleId;
    }
}

namespace UiGameSettings
{
bool CanEdit(uint64_t programId)
{
    if (programId == 0)
    {
        return false;
    }
    return !EmulationSession::GetInstance().OwnsGameTitle() || programId == RunningProgramId();
}

uint64_t RunningProgramId()
{
    if (!EmulationSession::GetInstance().OwnsGameTitle())
    {
        return 0;
    }
    const std::string titleId = Store().GetString(NXCoreSetting::GameTitleId);
    return std::strtoull(titleId.c_str(), nullptr, 16);
}

bool Open(uint64_t programId)
{
    Close();
    if (!CanEdit(programId))
    {
        return false;
    }
    if (!EmulationSession::GetInstance().OwnsGameTitle())
    {
        Store().SetString(NXCoreSetting::GameTitleId, TitleId(programId).c_str());
        s_titleIdSet = true;
    }
    s_programId = programId;
    return true;
}

void Close()
{
    if (s_titleIdSet && !EmulationSession::GetInstance().OwnsGameTitle())
    {
        Store().SetString(NXCoreSetting::GameTitleId, "");
    }
    s_titleIdSet = false;
    s_programId = 0;
}

bool Editing()
{
    return s_programId != 0;
}

int ReadInt(const char * key)
{
    return Editing() ? Store().GetStoredInt(ReadKey(key)) : Store().GetInt(ReadKey(key));
}

bool ReadBool(const char * key)
{
    return Editing() ? Store().GetStoredBool(ReadKey(key)) : Store().GetBool(ReadKey(key));
}

void SetInt(const char * key, int value)
{
    Store().SetInt(WriteKey(key), value);
}

void SetBool(const char * key, bool value)
{
    Store().SetBool(WriteKey(key), value);
}

bool Overridden(const char * key)
{
    const GameSetting * setting = Find(key);
    return Editing() && setting != nullptr && IsOverridden(*setting);
}

int OverriddenCount()
{
    int count = 0;
    for (const GameSetting & setting : kGameSettings)
    {
        count += IsOverridden(setting) ? 1 : 0;
    }
    return count;
}

void InheritAll()
{
    for (const GameSetting & setting : kGameSettings)
    {
        if (setting.isBool)
        {
            Store().SetBool(setting.game, Store().GetStoredBool(setting.system));
        }
        else
        {
            Store().SetInt(setting.game, Store().GetStoredInt(setting.system));
        }
    }
}
} // namespace UiGameSettings
