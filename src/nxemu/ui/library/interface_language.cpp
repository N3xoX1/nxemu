#include "interface_language.h"
#include <nxemu-core/settings/settings.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu/settings/ui_settings.h>
#include <nxemu/ui/emulation/emulator_session.h>
#include <nxemu/ui/host/system_language.h>
#include <nxemu/ui/i18n/translation.h>

std::string InterfaceLanguage::Choice()
{
    return Translation::IsSupportedLanguage(uiSettings.interfaceLanguage) ? uiSettings.interfaceLanguage : std::string();
}

std::string InterfaceLanguage::Code()
{
    return Translation::ResolveLanguage(SettingsStore::GetInstance().GetInt(NXOsSetting::LanguageIndex), Choice());
}

void InterfaceLanguage::SetChoice(const std::string & code)
{
    uiSettings.interfaceLanguage = code;
    SaveUISetting();
}

void InterfaceLanguage::Use(const std::string & code, const std::filesystem::path & assetDir)
{
    // From the source tree: src/nxemu/ui/assets -> lang
    Translation::UseLanguage(code, assetDir / "lang", assetDir.parent_path().parent_path().parent_path().parent_path() / "lang");
}

void InterfaceLanguage::AdoptSystemLanguage(EmulatorSession & session)
{
    if (uiSettings.consoleLanguageSet)
    {
        return;
    }
    SettingsStore & settings = SettingsStore::GetInstance();
    Language language{};
    if (settings.GetInt(NXOsSetting::LanguageIndex) == settings.GetDefaultInt(NXOsSetting::LanguageIndex) && SystemConsoleLanguage(language))
    {
        settings.SetInt(NXOsSetting::LanguageIndex, static_cast<int32_t>(language));
        session.FlushSettings();
    }
    uiSettings.consoleLanguageSet = true;
    SaveUISetting();
}
