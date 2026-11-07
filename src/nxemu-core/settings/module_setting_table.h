#pragma once
#include <common/json.h>
#include <cstddef>
#include <cstring>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-module-spec/base.h>
#include <string>

extern IModuleSettings * g_settings;

enum class StoreKind
{
    Bool,
    Int,
    Float,
    String,
};

namespace ModuleSettingTable
{
inline JsonValue ParseSettings(const std::string & json)
{
    JsonValue root;
    JsonReader reader;
    if (json.empty() || !reader.Parse(json.data(), json.data() + json.size(), root) || !root.isObject())
    {
        return JsonValue();
    }
    return root;
}

inline std::string SettingsJson(const JsonValue & root)
{
    return root.isNull() ? std::string() : JsonStyledWriter().write(root);
}

template <typename Setting, size_t N>
Setting * FindSetting(Setting (&settings)[N], const char * id)
{
    for (Setting & setting : settings)
    {
        if (setting.identifier != nullptr && strcmp(setting.identifier, id) == 0)
        {
            return &setting;
        }
    }
    return nullptr;
}

template <typename Setting>
bool SameStoreValue(const Setting & setting, const char * id, const char * other)
{
    switch (StoreKindOf(setting))
    {
    case StoreKind::Bool:
        return g_settings->GetBool(id) == g_settings->GetBool(other);
    case StoreKind::Float:
        return g_settings->GetFloat(id) == g_settings->GetFloat(other);
    case StoreKind::String:
        return strcmp(g_settings->GetString(id), g_settings->GetString(other)) == 0;
    default:
        return g_settings->GetInt(id) == g_settings->GetInt(other);
    }
}

template <typename Setting>
void CopyStoreSetting(const Setting & setting, const char * from, const char * to)
{
    switch (StoreKindOf(setting))
    {
    case StoreKind::Bool:
        g_settings->SetBool(to, g_settings->GetBool(from));
        break;
    case StoreKind::Float:
        g_settings->SetFloat(to, g_settings->GetFloat(from));
        break;
    case StoreKind::String:
        g_settings->SetString(to, std::string(g_settings->GetString(from)).c_str());
        break;
    default:
        g_settings->SetInt(to, g_settings->GetInt(from));
        break;
    }
}

template <typename Setting>
void KeepSavedSetting(const Setting & setting, const JsonValue & saved, JsonValue & root)
{
    const JsonValue value = JsonSettingValue(setting, saved);
    if (!value.isNull())
    {
        SetJsonSettingValue(setting, root, value);
    }
}

template <typename Setting, size_t N>
void SettingChanged(Setting (&settings)[N], const char * id)
{
    for (Setting & setting : settings)
    {
        if (setting.identifier == nullptr || strcmp(setting.identifier, id) != 0)
        {
            continue;
        }
        LoadStoreSetting(setting, id);
        if (setting.fallback != nullptr)
        {
            setting.gameValue = !SameStoreValue(setting, setting.identifier, setting.fallback);
        }
    }
    for (const Setting & setting : settings)
    {
        if (setting.fallback != nullptr && !setting.gameValue && strcmp(setting.fallback, id) == 0)
        {
            CopyStoreSetting(setting, id, setting.identifier);
        }
    }
}

template <typename Setting, size_t N>
void LoadGameSettings(Setting (&settings)[N], const char * section)
{
    const JsonValue root = ParseSettings(g_settings->GetGameSectionSettings(section));
    const bool debugEnabled = g_settings->DebugSettingsEnabled();
    for (Setting & setting : settings)
    {
        if (setting.fallback == nullptr)
        {
            continue;
        }
        const bool locked = setting.debugSetting && !debugEnabled;
        const bool present = !JsonSettingValue(setting, root).isNull();
        LoadStoreSetting(setting, setting.fallback);
        if (present && !locked)
        {
            ReadJsonSetting(setting, root);
        }
        StoreSetting(setting);
        setting.gameValue = present && !locked && !SameStoreValue(setting, setting.identifier, setting.fallback);
    }
}

template <typename Setting, size_t N>
void SetupSettings(Setting (&settings)[N], const char * section, SettingChangeCallback settingChanged, SettingChangeCallback gameSettingsChanged)
{
    for (Setting & setting : settings)
    {
        if (setting.fallback == nullptr)
        {
            continue;
        }
        if (const Setting * system = FindSetting(settings, setting.fallback))
        {
            InheritFallback(setting, *system);
        }
        g_settings->SetDebugSetting(setting.identifier, setting.debugSetting);
    }
    for (const Setting & setting : settings)
    {
        ApplyDefault(setting);
    }

    const JsonValue root = ParseSettings(g_settings->GetSectionSettings(section));
    for (const Setting & setting : settings)
    {
        if (setting.fallback == nullptr && IsPersisted(setting))
        {
            ReadJsonSetting(setting, root);
        }
    }
    for (const Setting & setting : settings)
    {
        if (setting.identifier == nullptr || setting.fallback != nullptr)
        {
            continue;
        }
        StoreSetting(setting);
        g_settings->SetDebugSetting(setting.identifier, setting.debugSetting);
        g_settings->RegisterCallback(setting.identifier, settingChanged, nullptr);
    }
    for (const Setting & setting : settings)
    {
        if (setting.debugSetting && !g_settings->DebugSettingsEnabled())
        {
            ApplyDefault(setting);
        }
    }

    LoadGameSettings(settings, section);
    for (Setting & setting : settings)
    {
        if (setting.fallback == nullptr)
        {
            continue;
        }
        g_settings->RegisterCallback(setting.identifier, settingChanged, nullptr);
        if (FindSetting(settings, setting.fallback) == nullptr)
        {
            g_settings->RegisterCallback(setting.fallback, settingChanged, nullptr);
        }
    }
    g_settings->RegisterCallback(NXCoreSetting::GameTitleId, gameSettingsChanged, nullptr);
    g_settings->RegisterCallback(NXCoreSetting::DebugSettingsEnabled, gameSettingsChanged, nullptr);
}

template <typename Setting, size_t N>
void CleanupSettings(Setting (&settings)[N], SettingChangeCallback settingChanged, SettingChangeCallback gameSettingsChanged)
{
    g_settings->UnregisterCallback(NXCoreSetting::GameTitleId, gameSettingsChanged, nullptr);
    g_settings->UnregisterCallback(NXCoreSetting::DebugSettingsEnabled, gameSettingsChanged, nullptr);
    for (Setting & setting : settings)
    {
        if (setting.identifier == nullptr)
        {
            continue;
        }
        g_settings->UnregisterCallback(setting.identifier, settingChanged, nullptr);
        if (setting.fallback != nullptr && FindSetting(settings, setting.fallback) == nullptr)
        {
            g_settings->UnregisterCallback(setting.fallback, settingChanged, nullptr);
        }
    }
}

template <typename Setting, size_t N>
void SaveSettings(Setting (&settings)[N], const char * section)
{
    const bool debugEnabled = g_settings->DebugSettingsEnabled();
    const JsonValue saved = ParseSettings(g_settings->GetSectionSettings(section));
    JsonValue root;
    for (const Setting & setting : settings)
    {
        if (setting.fallback != nullptr || !IsPersisted(setting))
        {
            continue;
        }
        if (setting.debugSetting && !debugEnabled)
        {
            KeepSavedSetting(setting, saved, root);
            continue;
        }
        WriteJsonSetting(setting, root, false);
    }
    g_settings->SetSectionSettings(section, SettingsJson(root).c_str());

    const JsonValue savedGame = ParseSettings(g_settings->GetGameSectionSettings(section));
    JsonValue gameRoot;
    for (const Setting & setting : settings)
    {
        if (setting.fallback == nullptr)
        {
            continue;
        }
        if (setting.debugSetting && !debugEnabled)
        {
            KeepSavedSetting(setting, savedGame, gameRoot);
            continue;
        }
        if (setting.gameValue && !SameStoreValue(setting, setting.identifier, setting.fallback))
        {
            WriteJsonSetting(setting, gameRoot, true);
        }
    }
    g_settings->SetGameSectionSettings(section, SettingsJson(gameRoot).c_str());
}
} // namespace ModuleSettingTable
