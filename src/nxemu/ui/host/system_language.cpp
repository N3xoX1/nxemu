#include "system_language.h"
#include <SDL.h>
#include <string>

namespace
{
bool IsOneOf(const std::string & country, std::initializer_list<const char *> countries)
{
    for (const char * candidate : countries)
    {
        if (country == candidate)
        {
            return true;
        }
    }
    return false;
}

bool ConsoleLanguageOf(const std::string & code, const std::string & country, Language & language)
{
    if (code == "ja")
    {
        language = Language::Japanese;
    }
    else if (code == "en")
    {
        language = IsOneOf(country, {"GB", "IE", "AU", "NZ"}) ? Language::EnglishBritish : Language::EnglishAmerican;
    }
    else if (code == "fr")
    {
        language = country == "CA" ? Language::FrenchCanadian : Language::French;
    }
    else if (code == "de")
    {
        language = Language::German;
    }
    else if (code == "it")
    {
        language = Language::Italian;
    }
    else if (code == "es")
    {
        language = country.empty() || country == "ES" ? Language::Spanish : Language::SpanishLatin;
    }
    else if (code == "zh")
    {
        language = IsOneOf(country, {"TW", "HK", "MO"}) ? Language::ChineseTraditional : Language::ChineseSimplified;
    }
    else if (code == "ko")
    {
        language = Language::Korean;
    }
    else if (code == "nl")
    {
        language = Language::Dutch;
    }
    else if (code == "pt")
    {
        language = country == "BR" ? Language::PortugueseBrazilian : Language::Portuguese;
    }
    else if (code == "ru")
    {
        language = Language::Russian;
    }
    else
    {
        return false;
    }
    return true;
}
} // namespace

bool SystemConsoleLanguage(Language & language)
{
    // The languages of the user, the preferred one first.
    SDL_Locale * locales = SDL_GetPreferredLocales();
    if (locales == nullptr)
    {
        return false;
    }
    bool found = false;
    for (const SDL_Locale * locale = locales; !found && locale->language != nullptr; ++locale)
    {
        found = ConsoleLanguageOf(locale->language, locale->country != nullptr ? locale->country : "", language);
    }
    SDL_free(locales);
    return found;
}
