#pragma once
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>

namespace Rml
{
class Context;
class ElementDocument;
} // namespace Rml

std::string Tr(const char * text);
std::string TrUpper(const std::string & text);
std::string Tr(const char * text, std::initializer_list<std::string> values);
std::string TrCount(const char * one, const char * other, uint64_t count);
constexpr const char * TrMark(const char * text)
{
    return text;
}

namespace Translation
{
struct LanguageInfo
{
    const char * code;
    const char * name; // In the language itself, so it can always be recognised.
};
inline constexpr LanguageInfo Languages[] = {
    {"de", "Deutsch"}, {"en", "English"}, {"es", "Español"}, {"fr", "Français"}, {"it", "Italiano"},
    {"nl", "Nederlands"}, {"pt", "Português"}, {"ru", "Русский"}, {"ja", "日本語"}, {"ko", "한국어"}, {"zh", "中文"},
};
bool IsSupportedLanguage(const std::string & code);
const char * LanguageOfConsole(int32_t language);
std::string ResolveLanguage(int32_t consoleLanguage, const std::string & overrideCode);
bool UseLanguage(const std::string & code, const std::filesystem::path & directory, const std::filesystem::path & fallbackDirectory);
bool Load(const std::filesystem::path & file);
void Clear();

Rml::ElementDocument * LoadDocument(Rml::Context & context, const std::filesystem::path & file);
void RefreshDocument(Rml::ElementDocument & document);
} // namespace Translation
