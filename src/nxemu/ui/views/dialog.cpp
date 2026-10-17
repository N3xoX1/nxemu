#include "dialog.h"
#include "rml_events.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <nxemu/ui/i18n/translation.h>
#include <yuzu_common/fs/fs_util.h>

namespace
{
constexpr const char * kCloseCommand = "close";
} // namespace

Dialog::Dialog(Rml::Context * context, std::filesystem::path file, float narrowBelow) :
    m_context(context),
    m_file(std::move(file)),
    m_narrowBelow(narrowBelow)
{
}

Dialog::~Dialog()
{
    Close();
}

void Dialog::Show()
{
    if (m_document != nullptr)
    {
        return;
    }
    m_document = Translation::LoadDocument(*m_context, m_file);
    if (m_document == nullptr)
    {
        return;
    }
    m_commands.clear();
    UiEvents::Listen(m_document, "click", [this](Rml::Event & event) {
        for (Rml::Element * element = event.GetTargetElement(); element != nullptr; element = element->GetParentNode())
        {
            if (element->HasAttribute("data-command"))
            {
                m_commands.push_back(element->GetAttribute<Rml::String>("data-command", ""));
                return;
            }
        }
    });
    UiEvents::Listen(m_document, "keydown", [this](Rml::Event & event) {
        if (event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN) == Rml::Input::KI_ESCAPE)
        {
            m_commands.push_back(kCloseCommand);
        }
    });
    m_document->SetClass("narrow", WindowWidth() < m_narrowBelow);
    m_document->SetClass("tight", WindowWidth() < kTightBelow);
    OnOpen();
    m_document->Show(Rml::ModalFlag::Modal);
}

void Dialog::Update()
{
    if (m_document == nullptr)
    {
        return;
    }
    m_document->SetClass("narrow", WindowWidth() < m_narrowBelow);
    m_document->SetClass("tight", WindowWidth() < kTightBelow);

    std::vector<std::string> commands;
    commands.swap(m_commands);
    for (const std::string & command : commands)
    {
        if (m_document == nullptr)
        {
            return;
        }
        if (command == kCloseCommand)
        {
            OnCloseRequested();
        }
        else
        {
            OnCommand(command);
        }
    }
    if (m_document != nullptr)
    {
        OnUpdate();
    }
}

void Dialog::RefreshTranslation()
{
    if (m_document != nullptr) Translation::RefreshDocument(*m_document);
}

float Dialog::WindowWidth() const
{
    return static_cast<float>(m_context->GetDimensions().x) / m_context->GetDensityIndependentPixelRatio();
}

void Dialog::Close()
{
    if (m_document != nullptr)
    {
        m_document->Close();
        m_document = nullptr;
    }
}

void Dialog::SetText(const char * id, const std::string & text)
{
    Rml::Element * element = m_document != nullptr ? m_document->GetElementById(id) : nullptr;
    if (element != nullptr && element->GetInnerRML() != text)
    {
        element->SetInnerRML(text);
    }
}
