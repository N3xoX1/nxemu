#include "settings_context.h"
#include <RmlUi/Core/ElementDocument.h>
#include <nxemu-core/settings/settings.h>

int32_t SettingsContext::GetInt(const char * key) const
{
    return SettingsStore::GetInstance().GetInt(key);
}

bool SettingsContext::GetBool(const char * key) const
{
    return SettingsStore::GetInstance().GetBool(key);
}

const char * SettingsContext::GetString(const char * key) const
{
    return SettingsStore::GetInstance().GetString(key);
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
    SettingsStore::GetInstance().SetInt(key, value);
}

void SettingsContext::SetBool(const char * key, bool value)
{
    SettingsStore::GetInstance().SetBool(key, value);
}

void SettingsContext::SetString(const char * key, const char * value)
{
    SettingsStore::GetInstance().SetString(key, value);
}

void SettingsContext::Decorate(Rml::ElementDocument * document) const
{
    document->SetClass("per-game", false);
    if (Rml::Element * banner = document->GetElementById("settings-scope"))
    {
        banner->SetInnerRML("");
        banner->SetClass("empty", true);
    }
}
