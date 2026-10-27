#include "translation.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <nxemu/ui/host/ui_pack.h>
#include <yuzu_common/fs/fs_util.h>

namespace
{
constexpr const char * kShownAttributes[] = {"title", "placeholder"};

bool Translated(const std::string & text, std::string & translated)
{
    std::string found;
    if (!Translation::Find(text, found) || found.find('<') != std::string::npos || found.find("{{") != std::string::npos)
    {
        return false;
    }
    translated = found;
    return true;
}

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
} // namespace

namespace Translation
{
bool UseLanguage(const std::string & code, const std::filesystem::path & directory, const std::filesystem::path & fallbackDirectory)
{
    if (code == "en") { Clear(); return true; }
    if (!IsSupportedLanguage(code)) return false;
    if (Load(directory / (code + ".po")) || Load(fallbackDirectory / (code + ".po"))) return true;
    Clear();
    return false;
}

bool Load(const std::filesystem::path & file)
{
    std::string text;
    if (!UiPack::ReadFile(file, text))
    {
        return false;
    }
    LoadCatalogue(text);
    return true;
}

Rml::ElementDocument * LoadDocument(Rml::Context & context, const std::filesystem::path & file)
{
    Rml::ElementDocument * document = context.LoadDocument(Common::FS::PathToUTF8String(file.generic_u8string()));
    if (document != nullptr) DocumentTexts(*document, true);
    return document;
}

void RefreshDocument(Rml::ElementDocument & document)
{
    DocumentTexts(document, false);
}
} // namespace Translation
