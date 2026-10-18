#include "settings_controls.h"
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/StringUtilities.h>
#include <cstdlib>

SettingsControls::Choice SettingsControls::Numbered(std::string name, int value, std::string hint)
{
    return {std::move(name), std::to_string(value), std::move(hint)};
}

Rml::ElementFormControlSelect * SettingsControls::Select(Rml::ElementDocument * document, const char * id)
{
    return rmlui_dynamic_cast<Rml::ElementFormControlSelect *>(document->GetElementById(id));
}

void SettingsControls::Fill(Rml::ElementFormControlSelect * select, const Choices & choices, const std::string & value)
{
    if (select == nullptr)
    {
        return;
    }
    select->RemoveAll();
    for (const Choice & choice : choices)
    {
        const int index = select->Add(Rml::StringUtilities::EncodeRml(choice.name), choice.value);
        Rml::Element * option = select->GetOption(index);
        if (option != nullptr && !choice.hint.empty())
        {
            option->SetAttribute("title", choice.hint);
        }
        if (choice.value == value)
        {
            select->SetSelection(index);
        }
    }
}

void SettingsControls::Choose(Rml::ElementFormControlSelect * select, const std::string & value)
{
    for (int index = 0; select != nullptr && index < select->GetNumOptions(); ++index)
    {
        Rml::Element * option = select->GetOption(index);
        if (option != nullptr && option->GetAttribute<Rml::String>("value", "") == value)
        {
            if (select->GetSelection() != index)
            {
                select->SetSelection(index);
            }
            return;
        }
    }
}

void SettingsControls::SetRange(Rml::ElementDocument * document, const char * id, int value)
{
    auto * input = rmlui_dynamic_cast<Rml::ElementFormControlInput *>(document->GetElementById(id));
    if (input != nullptr && std::atoi(input->GetValue().c_str()) != value)
    {
        input->SetValue(std::to_string(value));
    }
}

void SettingsControls::SetDisabled(Rml::ElementDocument * document, const char * id, bool disabled)
{
    auto * control = rmlui_dynamic_cast<Rml::ElementFormControl *>(document->GetElementById(id));
    if (control != nullptr && control->IsDisabled() != disabled)
    {
        control->SetDisabled(disabled);
    }
}

void SettingsControls::SetClass(Rml::ElementDocument * document, const char * id, const char * name, bool set)
{
    if (Rml::Element * element = document->GetElementById(id))
    {
        element->SetClass(name, set);
    }
}
