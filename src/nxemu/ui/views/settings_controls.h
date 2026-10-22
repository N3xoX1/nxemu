#pragma once
#include <string>
#include <vector>

namespace Rml
{
class ElementDocument;
class ElementFormControlSelect;
} // namespace Rml

// What the pages of the settings do with their controls.
namespace SettingsControls
{
// A value of a list, and what choosing it does: shown when the mouse rests on it.
struct Choice
{
    std::string name;
    std::string value;
    std::string hint;
};
using Choices = std::vector<Choice>;

Choice Numbered(std::string name, int value, std::string hint);

Rml::ElementFormControlSelect * Select(Rml::ElementDocument * document, const char * id);
// Shows the choices in the list, the one with the value chosen.
void Fill(Rml::ElementFormControlSelect * select, const Choices & choices, const std::string & value);
// Shows the choice with the value as the one chosen.
void Choose(Rml::ElementFormControlSelect * select, const std::string & value);
void SetRange(Rml::ElementDocument * document, const char * id, int value);
void SetDisabled(Rml::ElementDocument * document, const char * id, bool disabled);
void SetClass(Rml::ElementDocument * document, const char * id, const char * name, bool set);
} // namespace SettingsControls
