#include "system_language.h"
#include <SDL.h>
#include <nxemu-core/i18n/translation.h>

bool SystemConsoleLanguage(Language & language)
{
    SDL_Locale * locales = SDL_GetPreferredLocales();
    if (locales == nullptr)
    {
        return false;
    }
    bool found = false;
    int32_t console = 0;
    for (const SDL_Locale * locale = locales; !found && locale->language != nullptr; ++locale)
    {
        found = Translation::ConsoleLanguageOfLocale(locale->language, locale->country != nullptr ? locale->country : "", console);
    }
    SDL_free(locales);
    if (found)
    {
        language = static_cast<Language>(console);
    }
    return found;
}
