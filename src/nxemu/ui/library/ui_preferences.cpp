#include "ui_preferences.h"
#include <common/json.h>
#include <common/json_util.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <nxemu/ui/i18n/translation.h>

namespace
{
constexpr const char * kSection = "UI";
constexpr const char * kStatusOverrides = "StatusOverrides";
constexpr struct { const char * key; int min; int max; } kStatusInts[] = {
    {NXOsSetting::DockedMode, 0, 1}, {NXVideoSetting::ResolutionSetup, 0, 10},
    {NXVideoSetting::ScalingFilter, 0, 5}, {NXOsSetting::AudioVolume, 0, 200},
    {NXOsSetting::CurrentUser, 0, 7}, {NXVideoSetting::FSPSharpness, 0, 200},
};

bool ValidStatusInt(const char * setting, int32_t value)
{
    if (setting == nullptr) return false;
    for (const auto & entry : kStatusInts)
        if (std::strcmp(setting, entry.key) == 0) return value >= entry.min && value <= entry.max;
    return false;
}

JsonValue ReadValue(const char * key)
{
    return JsonGetNestedValue(SettingsStore::GetInstance().GetSettings(kSection), key);
}

void WriteValue(const char * key, const JsonValue & value)
{
    SettingsStore & settings = SettingsStore::GetInstance();
    JsonValue section = settings.GetSettings(kSection);
    JsonSetNestedValue(section, key, value);
    settings.SetSettings(kSection, section);
    settings.Save();
}
} // namespace

void UiPreferences::ApplyStatusOverrides()
{
    auto & settings = SettingsStore::GetInstance();
    const JsonValue overrides = ReadValue(kStatusOverrides);
    for (const auto & entry : kStatusInts)
    {
        const auto * value = overrides.Find(entry.key);
        if (value != nullptr && value->isInt() && value->asInt64() >= entry.min && value->asInt64() <= entry.max)
            settings.SetControlOverride(entry.key, *value);
    }
    const auto * muted = overrides.Find(NXOsSetting::AudioMuted);
    if (muted != nullptr && muted->isBool()) settings.SetControlOverride(NXOsSetting::AudioMuted, *muted);
}

void UiPreferences::SetStatusInt(const char * setting, int32_t value)
{
    if (!ValidStatusInt(setting, value)) return;
    auto & settings = SettingsStore::GetInstance();
    // Choosing what the game settings say is going back to them.
    if (settings.GetIntWithoutControl(setting) == value) return ResetStatusOverride(setting);
    settings.SetControlOverride(setting, JsonValue(value));
    JsonValue overrides = ReadValue(kStatusOverrides);
    overrides[setting] = value;
    WriteValue(kStatusOverrides, overrides);
}

void UiPreferences::SetStatusBool(const char * setting, bool value)
{
    if (setting == nullptr || std::strcmp(setting, NXOsSetting::AudioMuted) != 0) return;
    auto & settings = SettingsStore::GetInstance();
    if (settings.GetBoolWithoutControl(setting) == value) return ResetStatusOverride(setting);
    settings.SetControlOverride(setting, JsonValue(value));
    JsonValue overrides = ReadValue(kStatusOverrides);
    overrides[setting] = value;
    WriteValue(kStatusOverrides, overrides);
}

void UiPreferences::ResetStatusOverride(const char * setting)
{
    JsonValue overrides = ReadValue(kStatusOverrides);
    overrides.removeMember(setting);
    WriteValue(kStatusOverrides, overrides);
    SettingsStore::GetInstance().ClearControlOverride(setting);
}

namespace
{
// A choice saying what the game settings say changes nothing.
bool Overrides(const char * setting)
{
    const SettingsStore & store = SettingsStore::GetInstance();
    if (!store.HasControlOverride(setting)) return false;
    if (std::strcmp(setting, NXOsSetting::AudioMuted) == 0) return store.GetBool(setting) != store.GetBoolWithoutControl(setting);
    return store.GetInt(setting) != store.GetIntWithoutControl(setting);
}
} // namespace

bool UiPreferences::HasStatusOverride(std::initializer_list<const char *> settings)
{
    return std::any_of(settings.begin(), settings.end(), Overrides);
}

bool UiPreferences::HasStatusOverride()
{
    return Overrides(NXOsSetting::AudioMuted) || std::any_of(std::begin(kStatusInts), std::end(kStatusInts), [](const auto & entry) { return Overrides(entry.key); });
}

std::string UiPreferences::InterfaceLanguageChoice()
{
    const std::string code = GetString(InterfaceLanguage, "");
    return Translation::IsSupportedLanguage(code) ? code : std::string();
}

std::string UiPreferences::InterfaceLanguageCode()
{
    SettingsStore::GlobalReadScope global;
    return Translation::ResolveLanguage(SettingsStore::GetInstance().GetInt(NXOsSetting::LanguageIndex), InterfaceLanguageChoice());
}

std::string UiPreferences::GetString(const char * key, const std::string & defaultValue)
{
    const JsonValue value = ReadValue(key);
    return value.isString() ? value.asString() : defaultValue;
}

void UiPreferences::SetString(const char * key, const std::string & value)
{
    WriteValue(key, JsonValue(value));
}

std::vector<std::string> UiPreferences::GetStringList(const char * key)
{
    std::vector<std::string> result;
    const JsonValue values = ReadValue(key);
    if (values.isArray())
    {
        for (uint32_t i = 0; i < values.size(); i++)
        {
            if (values[i].isString())
            {
                result.push_back(values[i].asString());
            }
        }
    }
    return result;
}

void UiPreferences::SetStringList(const char * key, const std::vector<std::string> & values)
{
    JsonValue list(JsonValueType::Array);
    for (const std::string & value : values)
    {
        list.Append(JsonValue(value));
    }
    WriteValue(key, list);
}

bool UiPreferences::GetBool(const char * key, bool defaultValue)
{
    const JsonValue value = ReadValue(key);
    return value.isBool() ? value.asBool() : defaultValue;
}

void UiPreferences::SetBool(const char * key, bool value)
{
    WriteValue(key, JsonValue(value));
}

int32_t UiPreferences::GetInt(const char * key, int32_t defaultValue)
{
    const JsonValue value = ReadValue(key);
    return value.isInt() ? static_cast<int32_t>(value.asInt64()) : defaultValue;
}

void UiPreferences::SetInt(const char * key, int32_t value)
{
    WriteValue(key, JsonValue(value));
}
