#include "message_dialog.h"
#include "rml_events.h"
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>
#include <nxemu/ui/i18n/translation.h>

MessageDialog::MessageDialog(Rml::Context * context, const std::filesystem::path & assets,
                             bool question, std::string title, std::string text) :
    Dialog(context, assets / "message_dialog.rml"), m_question(question),
    m_title(std::move(title)), m_text(std::move(text)) {}

void MessageDialog::OnOpen()
{
    SetText("message-title", Rml::StringUtilities::EncodeRml(m_title));
    std::string text = Rml::StringUtilities::EncodeRml(m_text);
    for (size_t pos = 0; (pos = text.find('\n', pos)) != std::string::npos; pos += 5)
        text.replace(pos, 1, "<br/>");
    SetText("message-text", text);
    SetText("message-accept", Rml::StringUtilities::EncodeRml(Tr(m_question ? "Yes" : "OK")));
    auto * cancel = Document()->GetElementById("message-cancel");
    cancel->SetProperty("display", m_question ? "inline-block" : "none");
    (m_question ? cancel : Document()->GetElementById("message-accept"))->Focus();
    UiEvents::Listen(Document(), "keydown", [this](Rml::Event & event) {
        if (event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN) == Rml::Input::KI_RETURN)
        {
            // Enter follows the focused choice; questions initially focus No.
            m_enter = true;
        }
    });
}

void MessageDialog::OnUpdate()
{
    if (m_enter)
    {
        m_enter = false;
        auto * accept = Document()->GetElementById("message-accept");
        OnCommand(!m_question || accept->IsPseudoClassSet("focus") ? "accept" : "close");
    }
}

void MessageDialog::OnCommand(const std::string & command)
{
    if (command == "accept") m_response = NotificationResponse::Yes;
    Close();
}
