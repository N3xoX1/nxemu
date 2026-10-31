#include "dialog.h"
#include "rml_events.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <cmath>
#include <nxemu/ui/host/ui_dialog_window.h>
#include <nxemu/ui/host/ui_window.h>
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

Dialog::Dialog(UiWindow & window, std::filesystem::path file, float narrowBelow, int windowWidth, int windowHeight) :
    m_context(window.Context()),
    m_dialogWindow(window.DialogWindow()),
    m_file(std::move(file)),
    m_narrowBelow(narrowBelow),
    m_windowWidth(windowWidth),
    m_windowHeight(windowHeight)
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
    const bool inDialogWindow = m_dialogWindow != nullptr && m_dialogWindow->Prepare();
    m_document = Translation::LoadDocument(inDialogWindow ? *m_dialogWindow->Context() : *m_context, m_file);
    if (m_document == nullptr)
    {
        return;
    }
    if (inDialogWindow)
    {
        const Rml::Element * title = m_document->QuerySelector(".dialog-title");
        m_dialogWindow->Present(title != nullptr ? title->GetInnerRML() : m_document->GetTitle(), m_windowWidth, m_windowHeight);
        m_inDialogWindow = true;
        m_document->SetClass("own-window", true);
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
    if (m_inDialogWindow && m_windowHeight <= 0)
    {
        m_document->UpdateDocument();
        Rml::Element * panel = m_document->QuerySelector(".dialog");
        const float ratio = m_document->GetContext()->GetDensityIndependentPixelRatio();
        const float height = panel != nullptr ? panel->GetOffsetHeight() : m_document->GetOffsetHeight();
        m_dialogWindow->FitHeight(static_cast<int>(std::ceil(height / ratio)));
    }
}

void Dialog::Update()
{
    if (m_document == nullptr)
    {
        return;
    }
    m_document->SetClass("narrow", WindowWidth() < m_narrowBelow);
    m_document->SetClass("tight", WindowWidth() < kTightBelow);
    if (m_inDialogWindow && m_dialogWindow->TakeCloseRequest())
    {
        m_commands.push_back(kCloseCommand);
    }

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
    const Rml::Context * context = m_document != nullptr ? m_document->GetContext() : m_context;
    return static_cast<float>(context->GetDimensions().x) / context->GetDensityIndependentPixelRatio();
}

void Dialog::Close()
{
    if (m_document != nullptr)
    {
        m_document->Close();
        m_document = nullptr;
    }
    if (m_inDialogWindow)
    {
        m_inDialogWindow = false;
        m_dialogWindow->Dismiss();
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
