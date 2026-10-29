#include "ui_language.h"
#include "settings/ui_identifiers.h"
#include "ui_util.h"

#include <android/asset_manager.h>
#include <atomic>
#include <mutex>
#include <nxemu-core/i18n/translation.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/settings/core_settings.h>
#include <nxemu-core/settings/identifiers.h>

namespace
{
    std::mutex s_mutex;
    AAssetManager * s_assets = nullptr;
    std::string s_loaded = "en";
    std::atomic<int> s_generation = 0;

    bool ReadAsset(const std::string & path, std::string & text)
    {
        AAsset * asset = s_assets != nullptr ? AAssetManager_open(s_assets, path.c_str(), AASSET_MODE_BUFFER) : nullptr;
        if (asset == nullptr)
        {
            return false;
        }
        const auto * data = static_cast<const char *>(AAsset_getBuffer(asset));
        if (data != nullptr)
        {
            text.assign(data, static_cast<size_t>(AAsset_getLength(asset)));
        }
        AAsset_close(asset);
        return data != nullptr;
    }

    void AdoptDeviceLanguage(const std::string & locales)
    {
        if (UiUtil::ReadBool(NXUISetting::ConsoleLanguageSet))
        {
            return;
        }
        SettingsStore & settings = SettingsStore::GetInstance();
        if (settings.GetInt(NXCoreSetting::LanguageIndex) == settings.GetDefaultInt(NXCoreSetting::LanguageIndex))
        {
            for (size_t start = 0; start < locales.size();)
            {
                size_t end = locales.find(',', start);
                end = end == std::string::npos ? locales.size() : end;
                const std::string locale = locales.substr(start, end - start);
                const size_t separator = locale.find('_');
                int32_t language = 0;
                if (Translation::ConsoleLanguageOfLocale(locale.substr(0, separator), separator == std::string::npos ? "" : locale.substr(separator + 1), language))
                {
                    settings.SetInt(NXCoreSetting::LanguageIndex, language);
                    SaveCoreSetting();
                    break;
                }
                start = end + 1;
            }
        }
        UiUtil::SetBool(NXUISetting::ConsoleLanguageSet, true);
        UiUtil::SaveSettings();
    }
}

namespace UiLanguage
{
void Start(AAssetManager * assets, const std::string & deviceLocales)
{
    {
        std::lock_guard lock(s_mutex);
        s_assets = assets;
    }
    AdoptDeviceLanguage(deviceLocales);
    Apply();
}

void Apply()
{
    std::lock_guard lock(s_mutex);
    const std::string code = Code();
    if (code == s_loaded)
    {
        return;
    }
    std::string text;
    if (code != "en" && ReadAsset("ui/lang/" + code + ".po", text))
    {
        Translation::LoadCatalogue(text);
    }
    else
    {
        Translation::Clear();
    }
    s_loaded = code;
    s_generation++;
}

std::string Choice()
{
    const std::string choice = UiUtil::ReadString(NXUISetting::InterfaceLanguage);
    return Translation::IsSupportedLanguage(choice) ? choice : std::string();
}

void SetChoice(const std::string & code)
{
    UiUtil::SetString(NXUISetting::InterfaceLanguage, code);
    UiUtil::SaveSettings();
    Apply();
}

std::string Code()
{
    return Translation::ResolveLanguage(UiUtil::ReadInt(NXCoreSetting::LanguageIndex), Choice());
}

int Generation()
{
    return s_generation;
}
} // namespace UiLanguage
