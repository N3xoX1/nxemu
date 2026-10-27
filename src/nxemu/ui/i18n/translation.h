#pragma once
#include <filesystem>
#include <nxemu-core/i18n/translation.h>
#include <string>

namespace Rml
{
class Context;
class ElementDocument;
} // namespace Rml

namespace Translation
{
bool UseLanguage(const std::string & code, const std::filesystem::path & directory, const std::filesystem::path & fallbackDirectory);
bool Load(const std::filesystem::path & file);

Rml::ElementDocument * LoadDocument(Rml::Context & context, const std::filesystem::path & file);
void RefreshDocument(Rml::ElementDocument & document);
} // namespace Translation
