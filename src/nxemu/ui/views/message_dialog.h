#pragma once
#include "dialog.h"
#include <nxemu-core/notification.h>

class MessageDialog final : public Dialog
{
public:
    MessageDialog(Rml::Context * context, const std::filesystem::path & assets,
                  bool question, std::string title, std::string text);
    NotificationResponse Response() const { return m_response; }
    void Dismiss() { Close(); }
private:
    void OnOpen() override;
    void OnCommand(const std::string & command) override;
    void OnUpdate() override;
    bool m_question;
    bool m_enter = false;
    std::string m_title, m_text;
    NotificationResponse m_response = NotificationResponse::No;
};
