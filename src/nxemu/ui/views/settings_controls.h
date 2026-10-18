#pragma once
#include <string>
#include <vector>

namespace Rml
{
class ElementDocument;
class ElementFormControlSelect;
} // namespace Rml

namespace SettingsControls
{
struct Choice
{
    std::string name;
    std::string value;
    std::string hint;
};
using Choices = std::vector<Choice>;

Choice Numbered(std::string name, int value, std::string hint);

Rml::ElementFormControlSelect * Select(Rml::ElementDocument * document, const char * id);
void Fill(Rml::ElementFormControlSelect * select, const Choices & choices, const std::string & value);
void Choose(Rml::ElementFormControlSelect * select, const std::string & value);
void SetRange(Rml::ElementDocument * document, const char * id, int value);
void SetDisabled(Rml::ElementDocument * document, const char * id, bool disabled);
void SetClass(Rml::ElementDocument * document, const char * id, const char * name, bool set);
} // namespace SettingsControls
