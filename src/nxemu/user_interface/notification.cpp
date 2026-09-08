#include "notification.h"
#include "settings/ui_settings.h"
#include <sciter_element.h>
#include <sciter_handler.h>
#include <sciter_ui.h>
#include <common/std_string.h>

#ifdef WIN32
#include<Windows.h>
#endif

namespace
{
enum class NotificationDialogMode
{
    Alert,
    Query,
};

class NotificationWindow :
    public IClickSink,
    public IWindowDestroySink
{
public:
    NotificationWindow(ISciterUI & sciterUI) :
        m_sciterUI(sciterUI),
        m_window(nullptr),
        m_mode(NotificationDialogMode::Alert),
        m_response(NotificationResponse::No)
    {
    }

    NotificationResponse Show(void * parentWindow, NotificationDialogMode mode, const char * title, const char * message)
    {
        m_mode = mode;
        m_response = mode == NotificationDialogMode::Query ? NotificationResponse::No : NotificationResponse::Yes;
        m_window = nullptr;

        if (!m_sciterUI.WindowCreate(parentWindow, "notification_dialog.html", 0, 0, 0, 0, SUIW_CHILD, m_window))
        {
            return m_response;
        }

        SciterElement root(m_window->GetRootElement());
        if (root.IsValid())
        {
            SciterElement titleEl(root.GetElementByID("NotificationTitle"));
            if (titleEl.IsValid() && title != nullptr)
            {
                titleEl.SetText(title);
            }

            SciterElement messageEl(root.GetElementByID("NotificationMessage"));
            if (messageEl.IsValid() && message != nullptr)
            {
                messageEl.SetText(message);
            }

            ConfigureButtons();

            m_sciterUI.AttachHandler(root.FindFirst("button[role=\"window-ok\"]"), IID_ICLICKSINK, (IClickSink *)this);
            m_sciterUI.AttachHandler(root.FindFirst("button[role=\"window-close\"]"), IID_ICLICKSINK, (IClickSink *)this);
            m_sciterUI.AttachHandler(root.GetElementByID("NotificationYes"), IID_ICLICKSINK, (IClickSink *)this);
            m_sciterUI.AttachHandler(root.GetElementByID("NotificationNo"), IID_ICLICKSINK, (IClickSink *)this);
            m_sciterUI.AttachHandler(root.GetElementByID("NotificationOk"), IID_ICLICKSINK, (IClickSink *)this);
        }

        m_window->OnDestroySinkAdd(this);
        m_window->FixMinSize();
        m_window->CenterWindow();
        m_window->RunModal();

        return m_response;
    }

private:
    NotificationWindow(const NotificationWindow &) = delete;
    NotificationWindow & operator=(const NotificationWindow &) = delete;

    void ConfigureButtons()
    {
        SciterElement root(m_window->GetRootElement());
        if (!root.IsValid())
        {
            return;
        }

        const bool query = m_mode == NotificationDialogMode::Query;
        SciterElement yesBtn(root.GetElementByID("NotificationYes"));
        SciterElement noBtn(root.GetElementByID("NotificationNo"));
        SciterElement okBtn(root.GetElementByID("NotificationOk"));

        if (yesBtn.IsValid())
        {
            yesBtn.SetStyleAttribute("display", query ? "inline-block" : "none");
        }
        if (noBtn.IsValid())
        {
            noBtn.SetStyleAttribute("display", query ? "inline-block" : "none");
        }
        if (okBtn.IsValid())
        {
            okBtn.SetStyleAttribute("display", query ? "none" : "inline-block");
        }
    }

    void Close(NotificationResponse response)
    {
        if (m_window == nullptr || m_window->IsClosed())
        {
            return;
        }
        m_response = response;
        m_window->Destroy();
    }

    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/, uint32_t /*reason*/) override
    {
        SciterElement clickElem(element);
        const std::string role = clickElem.GetAttribute("role");
        const std::string id = clickElem.GetAttribute("id");

        if (id == "NotificationYes" || role == "window-ok")
        {
            Close(NotificationResponse::Yes);
            return true;
        }
        if (id == "NotificationNo" || role == "window-close")
        {
            Close(NotificationResponse::No);
            return true;
        }
        if (id == "NotificationOk")
        {
            Close(NotificationResponse::Yes);
            return true;
        }
        return false;
    }

    void OnWindowDestroy(HWINDOW /*hWnd*/) override
    {
        m_window = nullptr;
    }

    ISciterUI & m_sciterUI;
    ISciterWindow * m_window;
    NotificationDialogMode m_mode;
    NotificationResponse m_response;
};

} // namespace

std::unique_ptr<Notification> Notification::s_instance;

Notification::Notification() :
    m_sciterUI(nullptr),
    m_parentWindow(nullptr)
{
}

void Notification::SetSciterContext(ISciterUI * sciterUI, void * parentWindow)
{
    m_sciterUI = sciterUI;
    m_parentWindow = parentWindow;
}

void Notification::ClearSciterContext()
{
    m_sciterUI = nullptr;
    m_parentWindow = nullptr;
}

void Notification::DisplayError(const char * message, const char * title) const
{
    if (m_sciterUI == nullptr || m_parentWindow == nullptr)
    {
        return;
    }
    NotificationWindow dialog(*m_sciterUI);
    dialog.Show(m_parentWindow, NotificationDialogMode::Alert, title, message);
}

NotificationResponse Notification::Query(const char * message, const char * title) const
{
    if (m_sciterUI == nullptr || m_parentWindow == nullptr)
    {
        return NotificationResponse::No;
    }
    NotificationWindow dialog(*m_sciterUI);
    return dialog.Show(m_parentWindow, NotificationDialogMode::Query, title, message);
}

void Notification::BreakPoint(const char * fileName, uint32_t lineNumber)
{
    DisplayError(stdstr_f("Break point found at\n%s\n%d", fileName, lineNumber).c_str(), "Error");
#ifdef WIN32
    if (IsDebuggerPresent() != 0)
    {
        DebugBreak();
    }
#else
    __builtin_trap();
#endif
}

void Notification::AppInitDone(void)
{
    SetupUISetting();
}

Notification & Notification::GetInstance()
{
    if (s_instance == nullptr)
    {
        s_instance = std::make_unique<Notification>();
    }
    return *s_instance;
}

void Notification::CleanUp()
{
    s_instance.reset();
}
