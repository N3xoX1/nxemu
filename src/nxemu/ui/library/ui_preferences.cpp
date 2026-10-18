#include "ui_preferences.h"
#include <common/json.h>
#include <common/json_util.h>
#include <nxemu-core/settings/settings.h>

namespace
{
constexpr const char * kSection = "UI";

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
