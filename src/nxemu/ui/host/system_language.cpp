#include "system_language.h"
#include <SDL3/SDL.h>
#include <nxemu-core/i18n/translation.h>

bool SystemConsoleLanguage(Language & language)
{
    int count = 0;
    SDL_Locale ** locales = SDL_GetPreferredLocales(&count);
    if (locales == nullptr)
    {
        return false;
    }
    bool found = false;
    int32_t console = 0;
    for (int index = 0; !found && index < count; ++index)
    {
        const SDL_Locale * locale = locales[index];
        found = Translation::ConsoleLanguageOfLocale(locale->language, locale->country != nullptr ? locale->country : "", console);
    }
    SDL_free(locales);
    if (found)
    {
        language = static_cast<Language>(console);
    }
    return found;
}
