#include "module_settings.h"
#include "settings/settings.h"

const char * ModuleSettings::GetString(const char * setting) const
{
    return SettingsStore::GetInstance().GetString(setting);
}

bool ModuleSettings::GetBool(const char * setting) const
{
    return SettingsStore::GetInstance().GetBool(setting);
}

int32_t ModuleSettings::GetInt(const char* setting) const
{
    return SettingsStore::GetInstance().GetInt(setting);
}

float ModuleSettings::GetFloat(const char * setting) const
{
    return SettingsStore::GetInstance().GetFloat(setting);
}

void ModuleSettings::SetString(const char * setting, const char * value)
{
    SettingsStore::GetInstance().SetString(setting, value);
}

void ModuleSettings::SetBool(const char * setting, bool value)
{
    SettingsStore::GetInstance().SetBool(setting, value);
}

void ModuleSettings::SetInt(const char * setting, int32_t value)
{
    SettingsStore::GetInstance().SetInt(setting, value);
}

void ModuleSettings::SetFloat(const char * setting, float value)
{
    SettingsStore::GetInstance().SetFloat(setting, value);
}

void ModuleSettings::SetDefaultBool(const char * setting, bool value)
{
    SettingsStore::GetInstance().SetDefaultBool(setting, value);
}

void ModuleSettings::SetDefaultInt(const char * setting, int32_t value)
{
    SettingsStore::GetInstance().SetDefaultInt(setting, value);
}

void ModuleSettings::SetDefaultFloat(const char * setting, float value)
{
    SettingsStore::GetInstance().SetDefaultFloat(setting, value);
}

void ModuleSettings::SetDefaultString(const char * setting, const char * value)
{
    SettingsStore::GetInstance().SetDefaultString(setting, value);
}

const char * ModuleSettings::GetSectionSettings(const char * section) const
{
    JsonValue json = SettingsStore::GetInstance().GetSettings(section);
    m_sectionSetting = json.isNull() ? "" : JsonStyledWriter().write(json);
    return m_sectionSetting.c_str();
}

void ModuleSettings::SetSectionSettings(const char * section, const std::string & json)
{
    JsonValue root;
    if (!json.empty())
    {
        JsonReader reader;
        if (!reader.Parse(json.data(), json.data() + json.size(), root))
        {
            return;
        }
    }
    SettingsStore& settings = SettingsStore::GetInstance();
    settings.SetSettings(section, root);
    settings.Save();
}

void ModuleSettings::RegisterCallback(const char * setting, SettingChangeCallback callback, void * userData)
{
    SettingsStore::GetInstance().RegisterCallback(setting, callback, userData);
}

void ModuleSettings::UnregisterCallback(const char * setting, SettingChangeCallback callback, void * userData)
{
    SettingsStore::GetInstance().UnregisterCallback(setting, callback, userData);
}
