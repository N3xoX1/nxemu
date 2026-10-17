#include "translation.h"
#include "plural_rule.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <nxemu-os/os_types.h>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <yuzu_common/fs/fs_util.h>

namespace
{
// The attributes whose value is shown: an explanation, and the hint of an
// empty field.
constexpr const char * kShownAttributes[] = {"title", "placeholder"};

struct Catalogue
{
    std::unordered_map<std::string, std::string> texts;                // by English text
    std::unordered_map<std::string, std::vector<std::string>> counted; // by English text for one, the forms for a count
    PluralRule rule;
};

Catalogue & Current()
{
    static Catalogue catalogue;
    return catalogue;
}

// A text as the catalogue has it: spaces and line ends as one space, none
// at the ends. How a document is laid out in its file does not matter.
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

// The text between the quotes of a line of a catalogue, with its escapes
// read. False when the line has no quoted text.
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

// An entry of a catalogue, as it is read.
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
            // The heading of the catalogue: it tells how the language counts.
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

// The translation of a text of a document. False when it has none, or one
// that RmlUi would read as markup rather than show.
bool Translated(const std::string & text, std::string & translated)
{
    const Catalogue & catalogue = Current();
    if (catalogue.texts.empty())
    {
        return false;
    }
    const auto found = catalogue.texts.find(Normalised(text));
    if (found == catalogue.texts.end() || found->second.find('<') != std::string::npos || found->second.find("{{") != std::string::npos)
    {
        return false;
    }
    translated = found->second;
    return true;
}

// Keep English sources on the original DOM nodes. Replacing a node's
// contents in a view removes that source along with the old text node.
void DocumentTexts(Rml::Element & element, bool remember, bool upper = false)
{
    upper = upper || element.HasAttribute("data-upper");
    if (element.GetAttribute<Rml::String>("translate", "") == "no" || element.GetTagName() == "head" ||
        element.GetTagName() == "style" || element.GetTagName() == "script") return;
    if (auto * text = rmlui_dynamic_cast<Rml::ElementText *>(&element))
    {
        constexpr const char * sourceAttribute = "data-i18n-text";
        if (remember) text->SetAttribute(sourceAttribute, text->GetText());
        if (text->HasAttribute(sourceAttribute))
        {
            const std::string source = text->GetAttribute<Rml::String>(sourceAttribute, "");
            std::string translated;
            const std::string & shown = Translated(source, translated) ? translated : source;
            text->SetText(upper ? TrUpper(shown) : shown);
        }
    }
    for (const char * attribute : kShownAttributes)
    {
        const std::string sourceAttribute = std::string("data-i18n-") + attribute;
        if (remember && element.HasAttribute(attribute))
            element.SetAttribute(sourceAttribute, element.GetAttribute<Rml::String>(attribute, ""));
        if (element.HasAttribute(sourceAttribute))
        {
            const std::string source = element.GetAttribute<Rml::String>(sourceAttribute, "");
            std::string translated;
            element.SetAttribute(attribute, Translated(source, translated) ? translated : source);
        }
    }
    for (int index = 0; index < element.GetNumChildren(); ++index) DocumentTexts(*element.GetChild(index), remember, upper);
}

// The capital of a letter of the Latin, Greek and Cyrillic alphabets; any
// other character is returned as it is.
char32_t UpperLetter(char32_t letter)
{
    if (letter >= U'a' && letter <= U'z') return letter - 0x20;
    if (letter >= 0xE0 && letter <= 0xFE && letter != 0xF7) return letter - 0x20;
    if (letter == 0xFF) return 0x178;
    // Latin Extended-A: pairs of a capital and its small letter.
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
        // Letters with a capital take one or two bytes; the rest is copied.
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
    const Catalogue & catalogue = Current();
    const auto found = catalogue.texts.find(text);
    return found != catalogue.texts.end() ? found->second : std::string(text);
}

std::string Tr(const char * text, std::initializer_list<std::string> values)
{
    std::string result = Tr(text);
    // From the last, so that %1 does not take the start of %10.
    size_t number = values.size();
    for (auto value = values.end(); value != values.begin(); --number)
    {
        --value;
        Replace(result, "%" + std::to_string(number), *value);
    }
    return result;
}

std::string TrCount(const char * one, const char * other, uint64_t count)
{
    const Catalogue & catalogue = Current();
    const auto found = catalogue.counted.find(one);
    std::string result;
    if (found != catalogue.counted.end())
    {
        const size_t form = catalogue.rule.FormOf(count);
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

bool UseLanguage(const std::string & code, const std::filesystem::path & directory, const std::filesystem::path & fallbackDirectory)
{
    if (code == "en") { Clear(); return true; }
    if (!IsSupportedLanguage(code)) return false;
    if (Load(directory / (code + ".po")) || Load(fallbackDirectory / (code + ".po"))) return true;
    Clear();
    return false;
}

bool IsSupportedLanguage(const std::string & code)
{
    for (const LanguageInfo & language : Languages)
    {
        if (code == language.code)
        {
            return true;
        }
    }
    return false;
}

bool Load(const std::filesystem::path & file)
{
    std::ifstream stream(file);
    if (!stream)
    {
        return false;
    }
    Catalogue catalogue;
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
            // The English text for the other counts: the entry is found by the one for one.
            entry.counted = true;
            continued = nullptr;
        }
        else if (StartsWith(line, "msgid"))
        {
            entry.Store(catalogue);
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
            entry.Store(catalogue);
            entry = Entry{};
            entry.fuzzy = true;
            continued = nullptr;
        }
        else if (StartsWith(line, "\"") && continued != nullptr)
        {
            Quoted(line, *continued);
        }
    }
    entry.Store(catalogue);
    Current() = std::move(catalogue);
    return true;
}

void Clear()
{
    Current() = Catalogue{};
}

Rml::ElementDocument * LoadDocument(Rml::Context & context, const std::filesystem::path & file)
{
    // RmlUi finds the directory that a document's images are relative to by
    // forward slashes only.
    Rml::ElementDocument * document = context.LoadDocument(Common::FS::PathToUTF8String(file.generic_u8string()));
    if (document != nullptr) DocumentTexts(*document, true);
    return document;
}

void RefreshDocument(Rml::ElementDocument & document)
{
    DocumentTexts(document, false);
}
} // namespace Translation
