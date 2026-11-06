#include "settings_context.h"
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>
#include <cstdio>
#include <set>
#include <string_view>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/i18n/translation.h>

namespace
{
struct GameSetting
{
    const char * system;
    const char * game;
    const char * control;
    bool isBool;
};

constexpr GameSetting kGameSettings[] = {
    {NXOsSetting::SystemDockedMode, NXOsSetting::GameDockedMode, "mode-docked", false},
    {NXOsSetting::SystemMemoryLayout, NXOsSetting::GameMemoryLayout, "memory", false},
    {NXOsSetting::SystemRegion, NXOsSetting::GameRegion, "region", false},
    {NXCoreSetting::SystemLanguageIndex, NXOsSetting::GameLanguageIndex, "language", false},
    {NXOsSetting::SystemSpeedLimit, NXOsSetting::GameSpeedLimit, "speed-range", false},
    {NXOsSetting::SystemUseMultiCore, NXOsSetting::GameUseMultiCore, "multicore", true},
    {NXOsSetting::SystemCpuHleSynchronization, NXOsSetting::GameCpuHleSynchronization, "cpu-hle", true},
    {NXOsSetting::SystemAudioMode, NXOsSetting::GameAudioMode, "audio-mode", false},
    {NXOsSetting::SystemVibrationEnabled, NXOsSetting::GameVibrationEnabled, "vibration", true},
    {NXOsSetting::SystemEnableAccurateVibrations, NXOsSetting::GameEnableAccurateVibrations, "accurate-vibration", true},
    {NXVideoSetting::SystemUseDiskPipelineCache, NXVideoSetting::GameUseDiskPipelineCache, "disk-cache", true},
    {NXVideoSetting::SystemUseAsynchronousGPUEmulation, NXVideoSetting::GameUseAsynchronousGPUEmulation, "async-gpu", true},
    {NXVideoSetting::SystemAstcDecodeMode, NXVideoSetting::GameAstcDecodeMode, "astc", false},
    {NXVideoSetting::SystemEnableAsynchronousPresentation, NXVideoSetting::GameEnableAsynchronousPresentation, "async-present", true},
    {NXVideoSetting::SystemEnableReactiveFlushing, NXVideoSetting::GameEnableReactiveFlushing, "reactive-flushing", true},
    {NXVideoSetting::SystemAccuracyLevel, NXVideoSetting::GameAccuracyLevel, "accuracy", false},
    {NXVideoSetting::SystemAnisotropicFiltering, NXVideoSetting::GameAnisotropicFiltering, "anisotropy", false},
    {NXVideoSetting::SystemDMAAccuracy, NXVideoSetting::GameDMAAccuracy, "dma", false},
    {NXVideoSetting::SystemGpuCommandSynchronization, NXVideoSetting::GameGpuCommandSynchronization, "gpu-sync", true},
    {NXVideoSetting::SystemVSyncMode, NXVideoSetting::GameVSyncMode, "vsync", false},
    {NXVideoSetting::SystemNvdecEmulation, NXVideoSetting::GameNvdecEmulation, "nvdec", false},
    {NXVideoSetting::SystemAspectRatio, NXVideoSetting::GameAspectRatio, "aspect", false},
    {NXVideoSetting::SystemResolutionSetup, NXVideoSetting::GameResolutionSetup, "resolution", false},
    {NXVideoSetting::SystemScalingFilter, NXVideoSetting::GameScalingFilter, "filter", false},
    {NXVideoSetting::SystemAntiAliasing, NXVideoSetting::GameAntiAliasing, "antialiasing", false},
    {NXVideoSetting::SystemFSPSharpness, NXVideoSetting::GameFSPSharpness, "sharpness-range", false},
    {NXVideoSetting::SystemForceMaximumClocks, NXVideoSetting::GameForceMaximumClocks, "max-clocks", true},
    {NXVideoSetting::SystemUseAsynchronousShaderBuilding, NXVideoSetting::GameUseAsynchronousShaderBuilding, "async-shaders", true},
    {NXVideoSetting::SystemFastGPUTime, NXVideoSetting::GameFastGPUTime, "fast-gpu-time", true},
    {NXVideoSetting::SystemUseVulkanPipelineCache, NXVideoSetting::GameUseVulkanPipelineCache, "driver-cache", true},
    {NXVideoSetting::SystemSyncToFramerateOfVideoPlayback, NXVideoSetting::GameSyncToFramerateOfVideoPlayback, "video-framerate", true},
    {NXVideoSetting::SystemBarrierFeedbackLoops, NXVideoSetting::GameBarrierFeedbackLoops, "feedback-loops", true},
    {NXVideoSetting::SystemASTCRecompressionMethod, NXVideoSetting::GameASTCRecompressionMethod, "astc-recompression", false},
    {NXVideoSetting::SystemVRAMUsageMode, NXVideoSetting::GameVRAMUsageMode, "vram", false},
    {NXCpuSetting::SystemCpuBackend, NXCpuSetting::GameCpuBackend, "cpu-backend", false},
    {NXCpuSetting::SystemCpuAccuracy, NXCpuSetting::GameCpuAccuracy, "cpu-accuracy", false},
};

const GameSetting * FindGameSetting(const char * key)
{
    for (const GameSetting & setting : kGameSettings)
    {
        if (std::string_view(setting.system) == key || std::string_view(setting.game) == key)
        {
            return &setting;
        }
    }
    return nullptr;
}

bool IsOverridden(const GameSetting & setting)
{
    const SettingsStore & store = SettingsStore::GetInstance();
    if (setting.isBool)
    {
        return store.GetStoredBool(setting.game) != store.GetStoredBool(setting.system);
    }
    return store.GetStoredInt(setting.game) != store.GetStoredInt(setting.system);
}

void Inherit(const GameSetting & setting)
{
    SettingsStore & store = SettingsStore::GetInstance();
    if (setting.isBool)
    {
        store.SetBool(setting.game, store.GetStoredBool(setting.system));
    }
    else
    {
        store.SetInt(setting.game, store.GetStoredInt(setting.system));
    }
}

std::string TitleId(uint64_t programId)
{
    char titleId[17];
    std::snprintf(titleId, sizeof(titleId), "%016llX", (unsigned long long)programId);
    return titleId;
}

bool GameRunning()
{
    return SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning);
}

const char * RunningKey(const char * key)
{
    const GameSetting * setting = FindGameSetting(key);
    if (setting == nullptr)
    {
        return key;
    }
    return GameRunning() ? setting->game : setting->system;
}

const char * WriteKey(const char * key)
{
    const GameSetting * setting = FindGameSetting(key);
    if (setting == nullptr)
    {
        return key;
    }
    return GameRunning() && IsOverridden(*setting) ? setting->game : setting->system;
}
} // namespace

const char * GameSettingId(const char * key)
{
    const GameSetting * setting = FindGameSetting(key);
    return setting != nullptr ? setting->game : nullptr;
}

int32_t RunningGameSettings::GetInt(const char * key) const
{
    return SettingsStore::GetInstance().GetInt(RunningKey(key));
}

bool RunningGameSettings::GetBool(const char * key) const
{
    return SettingsStore::GetInstance().GetBool(RunningKey(key));
}

void RunningGameSettings::SetInt(const char * key, int32_t value)
{
    SettingsStore::GetInstance().SetInt(WriteKey(key), value);
}

void RunningGameSettings::SetBool(const char * key, bool value)
{
    SettingsStore::GetInstance().SetBool(WriteKey(key), value);
}

SettingsContext::SettingsContext(EmulatorSession & session) :
    m_session(session)
{
}

void SettingsContext::Global()
{
    if (m_titleIdSet && !m_session.OwnsGameTitle())
    {
        SettingsStore::GetInstance().SetString(NXCoreSetting::GameTitleId, "");
    }
    m_programId = 0;
    m_title.clear();
    m_titleIdSet = false;
}

bool SettingsContext::Game(uint64_t programId, std::string title)
{
    Global();
    if (!CanConfigure(programId))
    {
        return false;
    }
    if (!m_session.OwnsGameTitle())
    {
        SettingsStore::GetInstance().SetString(NXCoreSetting::GameTitleId, TitleId(programId).c_str());
        m_titleIdSet = true;
    }
    m_programId = programId;
    m_title = std::move(title);
    return true;
}

bool SettingsContext::CanConfigure(uint64_t programId) const
{
    if (programId == 0)
    {
        return false;
    }
    return !m_session.OwnsGameTitle() || TitleId(programId) == SettingsStore::GetInstance().GetString(NXCoreSetting::GameTitleId);
}

bool SettingsContext::Editable() const
{
    return !IsGame() || TitleId(m_programId) == SettingsStore::GetInstance().GetString(NXCoreSetting::GameTitleId);
}

std::string SettingsContext::Title() const
{
    return IsGame() ? Tr("Settings for %1", {m_title}) + " (" + TitleId(m_programId) + ")" : Tr("Settings");
}

int32_t SettingsContext::GetInt(const char * key) const
{
    const char * gameKey = IsGame() ? GameSettingId(key) : nullptr;
    return SettingsStore::GetInstance().GetStoredInt(gameKey != nullptr ? gameKey : key);
}

bool SettingsContext::GetBool(const char * key) const
{
    const char * gameKey = IsGame() ? GameSettingId(key) : nullptr;
    return SettingsStore::GetInstance().GetStoredBool(gameKey != nullptr ? gameKey : key);
}

const char * SettingsContext::GetString(const char * key) const
{
    return SettingsStore::GetInstance().GetStoredString(key);
}

int32_t SettingsContext::GetDefaultInt(const char * key) const
{
    return SettingsStore::GetInstance().GetDefaultInt(key);
}

bool SettingsContext::GetDefaultBool(const char * key) const
{
    return SettingsStore::GetInstance().GetDefaultBool(key);
}

const char * SettingsContext::GetDefaultString(const char * key) const
{
    return SettingsStore::GetInstance().GetDefaultString(key);
}

void SettingsContext::SetInt(const char * key, int32_t value)
{
    if (!IsGame())
    {
        SettingsStore::GetInstance().SetInt(key, value);
    }
    else if (const char * gameKey = GameSettingId(key); gameKey != nullptr && Editable())
    {
        SettingsStore::GetInstance().SetInt(gameKey, value);
    }
}

void SettingsContext::SetBool(const char * key, bool value)
{
    if (!IsGame())
    {
        SettingsStore::GetInstance().SetBool(key, value);
    }
    else if (const char * gameKey = GameSettingId(key); gameKey != nullptr && Editable())
    {
        SettingsStore::GetInstance().SetBool(gameKey, value);
    }
}

void SettingsContext::SetString(const char * key, const char * value)
{
    if (!IsGame())
    {
        SettingsStore::GetInstance().SetString(key, value);
    }
}

bool SettingsContext::InheritCommand(const std::string & command, const SettingChangeable & changeable)
{
    if (!IsGame() || command.rfind("inherit:", 0) != 0)
    {
        return false;
    }
    const GameSetting * setting = FindGameSetting(command.c_str() + 8);
    if (setting != nullptr && Editable() && changeable(setting->system))
    {
        Inherit(*setting);
    }
    return true;
}

void SettingsContext::InheritAll(const SettingChangeable & changeable)
{
    if (!IsGame() || !Editable())
    {
        return;
    }
    for (const GameSetting & setting : kGameSettings)
    {
        if (changeable(setting.system))
        {
            Inherit(setting);
        }
    }
}

void SettingsContext::Decorate(Rml::ElementDocument * document, const SettingChangeable & changeable) const
{
    document->SetClass("per-game", IsGame());
    if (Rml::Element * banner = document->GetElementById("settings-scope"))
    {
        banner->SetInnerRML("");
        banner->SetClass("empty", true);
    }
    Rml::ElementList globalOnly;
    document->GetElementsByClassName(globalOnly, "global-only");
    for (Rml::Element * element : globalOnly)
    {
        if (IsGame())
        {
            element->SetProperty("display", "none");
        }
        else
        {
            element->RemoveProperty("display");
        }
    }
    Rml::ElementList rows;
    document->GetElementsByClassName(rows, "row");
    std::set<Rml::Element *> gameRows;
    for (Rml::Element * row : rows)
    {
        row->SetClass("setting-overridden", false);
        row->SetClass("game-locked", false);
    }
    if (!IsGame())
    {
        return;
    }
    for (const GameSetting & setting : kGameSettings)
    {
        Rml::Element * control = document->GetElementById(setting.control);
        Rml::Element * row = control;
        while (row != nullptr && !row->IsClassSet("row"))
        {
            row = row->GetParentNode();
        }
        if (row == nullptr)
        {
            continue;
        }
        gameRows.insert(row);
        const std::string id = std::string("source:") + setting.system;
        Rml::Element * status = document->GetElementById(id);
        if (status == nullptr)
        {
            Rml::ElementList origins;
            row->GetElementsByClassName(origins, "setting-origins");
            Rml::Element * footer = origins.empty() ? nullptr : origins.front();
            if (footer == nullptr)
            {
                Rml::ElementPtr element = document->CreateElement("div");
                element->SetClass("setting-origins", true);
                footer = row->AppendChild(std::move(element));
            }
            Rml::ElementPtr element = document->CreateElement("div");
            element->SetId(id);
            element->SetClass("setting-origin", true);
            status = footer->AppendChild(std::move(element));
        }
        const bool overridden = IsOverridden(setting);
        if (overridden)
        {
            row->SetClass("setting-overridden", true);
        }
        status->SetClass("source-global", !overridden);
        status->SetClass("source-user", overridden);
        status->SetInnerRML(overridden ? "<span class=\"origin-label\">" + Rml::StringUtilities::EncodeRml(Tr("Game overrides the system setting")) +
                                             "</span><button class=\"inherit-setting\" data-command=\"inherit:" + std::string(setting.system) + "\"" +
                                             (changeable(setting.system) ? "" : " disabled") + "><div>" +
                                             Rml::StringUtilities::EncodeRml(Tr("Inherit")) + "</div></button>"
                                       : std::string());
    }
    for (Rml::Element * row : rows)
    {
        row->SetClass("game-locked", gameRows.count(row) == 0);
    }
}
