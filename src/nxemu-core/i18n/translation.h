#pragma once
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>

std::string Tr(const char * text);
std::string TrUpper(const std::string & text);
std::string Tr(const char * text, std::initializer_list<std::string> values);
std::string Tr(const char * text, std::span<const std::string> values);
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
// Indexed by Language.
inline constexpr const char * ConsoleLanguages[] = {
    "日本語", "English (US)", "Français", "Deutsch", "Italiano", "Español", "中文", "한국어", "Nederlands",
    "Português", "Русский", "中文（台灣）", "English (UK)", "Français (Canada)", "Español (Latinoamérica)",
    "中文（简体）", "中文（繁體）", "Português (Brasil)",
};
bool IsSupportedLanguage(const std::string & code);
const char * LanguageName(const std::string & code);
const char * LanguageOfConsole(int32_t language);
std::string ResolveLanguage(int32_t consoleLanguage, const std::string & overrideCode);
bool ConsoleLanguageOfLocale(const std::string & code, const std::string & country, int32_t & language);

void LoadCatalogue(const std::string & poText);
void Clear();
bool Find(const std::string & text, std::string & translated);
} // namespace Translation
