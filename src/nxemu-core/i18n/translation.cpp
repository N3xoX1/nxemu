#include "translation.h"
#include "plural_rule.h"
#include <memory>
#include <mutex>
#include <nxemu-os/os_types.h>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
struct Catalogue
{
    std::unordered_map<std::string, std::string> texts;                // by English text
    std::unordered_map<std::string, std::vector<std::string>> counted; // by English text for one, the forms for a count
    PluralRule rule;
};

struct Shared
{
    std::mutex mutex;
    std::shared_ptr<const Catalogue> catalogue = std::make_shared<const Catalogue>();
};

Shared & State()
{
    static Shared shared;
    return shared;
}

std::shared_ptr<const Catalogue> Current()
{
    Shared & shared = State();
    std::lock_guard lock(shared.mutex);
    return shared.catalogue;
}

void Use(std::shared_ptr<const Catalogue> catalogue)
{
    Shared & shared = State();
    std::lock_guard lock(shared.mutex);
    shared.catalogue = std::move(catalogue);
}

std::string Normalised(const std::string & text)
{
    std::string result;
    result.reserve(text.size());
    bool space = false;
    for (const char character : text)
    {
        if (character == ' ' || character == '\t' || character == '\n' || character == '\r')
        {
            space = !result.empty();
            continue;
        }
        if (space)
        {
            result += ' ';
            space = false;
        }
        result += character;
    }
    return result;
}

void Replace(std::string & text, const std::string & what, const std::string & with)
{
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size()))
    {
        text.replace(at, what.size(), with);
    }
}

bool Quoted(const std::string & line, std::string & text)
{
    const size_t first = line.find('"');
    const size_t last = line.rfind('"');
    if (first == std::string::npos || last == first)
    {
        return false;
    }
    for (size_t at = first + 1; at < last; ++at)
    {
        if (line[at] != '\\' || at + 1 == last)
        {
            text += line[at];
            continue;
        }
        const char escaped = line[++at];
        text += escaped == 'n' ? '\n' : escaped == 't' ? '\t' : escaped;
    }
    return true;
}

struct Entry
{
    std::string text;
    bool counted = false;
    std::vector<std::string> forms;
    bool fuzzy = false; // a translation its translator has yet to confirm

    void Store(Catalogue & catalogue) const
    {
        if (forms.empty() || forms[0].empty() || fuzzy)
        {
            return;
        }
        if (text.empty())
        {
            const size_t at = forms[0].find("Plural-Forms:");
            if (at != std::string::npos)
            {
                catalogue.rule = PluralRule(forms[0].substr(at, forms[0].find('\n', at) - at));
            }
        }
        else if (counted)
        {
            catalogue.counted[text] = forms;
        }
        else
        {
            catalogue.texts[text] = forms[0];
        }
    }
};

bool StartsWith(const std::string & line, const char * prefix)
{
    return line.rfind(prefix, 0) == 0;
}

char32_t UpperLetter(char32_t letter)
{
    if (letter >= U'a' && letter <= U'z') return letter - 0x20;
    if (letter >= 0xE0 && letter <= 0xFE && letter != 0xF7) return letter - 0x20;
    if (letter == 0xFF) return 0x178;
    if ((letter >= 0x100 && letter <= 0x137) || (letter >= 0x14A && letter <= 0x177)) return letter & ~char32_t(1);
    if ((letter >= 0x139 && letter <= 0x148) || (letter >= 0x179 && letter <= 0x17E)) return letter % 2 == 0 ? letter - 1 : letter;
    if (letter == 0x3C2) return 0x3A3;
    if (letter >= 0x3B1 && letter <= 0x3C9) return letter - 0x20;
    if (letter >= 0x430 && letter <= 0x44F) return letter - 0x20;
    if (letter >= 0x450 && letter <= 0x45F) return letter - 0x50;
    return letter;
}
} // namespace

std::string TrUpper(const std::string & text)
{
    std::string upper;
    upper.reserve(text.size());
    for (size_t index = 0; index < text.size();)
    {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        const bool pair = (lead & 0xE0) == 0xC0 && index + 1 < text.size();
        if (lead >= 0x80 && !pair)
        {
            upper += text[index++];
            continue;
        }
        const char32_t letter = UpperLetter(pair ? ((lead & 0x1F) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3F) : lead);
        if (letter < 0x80)
        {
            upper += static_cast<char>(letter);
        }
        else
        {
            upper += static_cast<char>(0xC0 | (letter >> 6));
            upper += static_cast<char>(0x80 | (letter & 0x3F));
        }
        index += pair ? 2 : 1;
    }
    return upper;
}

std::string Tr(const char * text)
{
    const std::shared_ptr<const Catalogue> catalogue = Current();
    const auto found = catalogue->texts.find(text);
    return found != catalogue->texts.end() ? found->second : std::string(text);
}

std::string Tr(const char * text, std::initializer_list<std::string> values)
{
    return Tr(text, std::span<const std::string>(values.begin(), values.size()));
}

std::string Tr(const char * text, std::span<const std::string> values)
{
    std::string result = Tr(text);
    // The last first, so %1 is not taken for the start of %10.
    for (size_t number = values.size(); number != 0; --number)
    {
        Replace(result, "%" + std::to_string(number), values[number - 1]);
    }
    return result;
}

std::string TrCount(const char * one, const char * other, uint64_t count)
{
    const std::shared_ptr<const Catalogue> catalogue = Current();
    const auto found = catalogue->counted.find(one);
    std::string result;
    if (found != catalogue->counted.end())
    {
        const size_t form = catalogue->rule.FormOf(count);
        result = form < found->second.size() && !found->second[form].empty() ? found->second[form] : found->second[0];
    }
    else
    {
        result = count == 1 ? one : other;
    }
    Replace(result, "%n", std::to_string(count));
    return result;
}

namespace Translation
{
const char * LanguageOfConsole(int32_t language)
{
    switch (static_cast<Language>(language))
    {
    case Language::Japanese: return "ja";
    case Language::EnglishAmerican:
    case Language::EnglishBritish: return "en";
    case Language::French:
    case Language::FrenchCanadian: return "fr";
    case Language::German: return "de";
    case Language::Italian: return "it";
    case Language::Spanish:
    case Language::SpanishLatin: return "es";
    case Language::Chinese:
    case Language::Taiwanese:
    case Language::ChineseSimplified:
    case Language::ChineseTraditional: return "zh";
    case Language::Korean: return "ko";
    case Language::Dutch: return "nl";
    case Language::Portuguese:
    case Language::PortugueseBrazilian: return "pt";
    case Language::Russian: return "ru";
    default: return "en";
    }
}

std::string ResolveLanguage(int32_t consoleLanguage, const std::string & overrideCode)
{
    return IsSupportedLanguage(overrideCode) ? overrideCode : LanguageOfConsole(consoleLanguage);
}

bool IsSupportedLanguage(const std::string & code)
{
    return LanguageName(code) != nullptr;
}

const char * LanguageName(const std::string & code)
{
    for (const LanguageInfo & language : Languages)
    {
        if (code == language.code)
        {
            return language.name;
        }
    }
    return nullptr;
}

bool ConsoleLanguageOfLocale(const std::string & code, const std::string & country, int32_t & language)
{
    const auto isOneOf = [&country](std::initializer_list<const char *> countries) {
        for (const char * candidate : countries)
        {
            if (country == candidate)
            {
                return true;
            }
        }
        return false;
    };
    Language console;
    if (code == "ja")
    {
        console = Language::Japanese;
    }
    else if (code == "en")
    {
        console = isOneOf({"GB", "IE", "AU", "NZ"}) ? Language::EnglishBritish : Language::EnglishAmerican;
    }
    else if (code == "fr")
    {
        console = country == "CA" ? Language::FrenchCanadian : Language::French;
    }
    else if (code == "de")
    {
        console = Language::German;
    }
    else if (code == "it")
    {
        console = Language::Italian;
    }
    else if (code == "es")
    {
        console = country.empty() || country == "ES" ? Language::Spanish : Language::SpanishLatin;
    }
    else if (code == "zh")
    {
        console = isOneOf({"TW", "HK", "MO"}) ? Language::ChineseTraditional : Language::ChineseSimplified;
    }
    else if (code == "ko")
    {
        console = Language::Korean;
    }
    else if (code == "nl")
    {
        console = Language::Dutch;
    }
    else if (code == "pt")
    {
        console = country == "BR" ? Language::PortugueseBrazilian : Language::Portuguese;
    }
    else if (code == "ru")
    {
        console = Language::Russian;
    }
    else
    {
        return false;
    }
    language = static_cast<int32_t>(console);
    return true;
}

void LoadCatalogue(const std::string & poText)
{
    std::istringstream stream(poText);
    auto catalogue = std::make_shared<Catalogue>();
    Entry entry;
    std::string * continued = nullptr; // what a line of quoted text alone adds to
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (StartsWith(line, "msgid_plural"))
        {
            entry.counted = true;
            continued = nullptr;
        }
        else if (StartsWith(line, "msgid"))
        {
            entry.Store(*catalogue);
            const bool fuzzy = entry.fuzzy && entry.forms.empty();
            entry = Entry{};
            entry.fuzzy = fuzzy;
            Quoted(line, entry.text);
            continued = &entry.text;
        }
        else if (StartsWith(line, "msgstr"))
        {
            entry.forms.emplace_back();
            Quoted(line, entry.forms.back());
            continued = &entry.forms.back();
        }
        else if (StartsWith(line, "#,") && line.find("fuzzy") != std::string::npos)
        {
            entry.Store(*catalogue);
            entry = Entry{};
            entry.fuzzy = true;
            continued = nullptr;
        }
        else if (StartsWith(line, "\"") && continued != nullptr)
        {
            Quoted(line, *continued);
        }
    }
    entry.Store(*catalogue);
    Use(std::move(catalogue));
}

void Clear()
{
    Use(std::make_shared<const Catalogue>());
}

bool Find(const std::string & text, std::string & translated)
{
    const std::shared_ptr<const Catalogue> catalogue = Current();
    if (catalogue->texts.empty())
    {
        return false;
    }
    const auto found = catalogue->texts.find(Normalised(text));
    if (found == catalogue->texts.end())
    {
        return false;
    }
    translated = found->second;
    return true;
}
} // namespace Translation
