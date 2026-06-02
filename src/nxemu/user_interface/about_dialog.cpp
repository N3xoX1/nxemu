#include "about_dialog.h"

#include <common/shell_open.h>
#include <nxemu-core/version.h>
#include <sciter_element.h>
#include <sciter_handler.h>
#include <sciter_ui.h>

#include <string>

namespace
{

void AttachClickHandler(ISciterUI & sciterUI, const SciterElement & element, IClickSink * sink)
{
    if (element.IsValid())
    {
        sciterUI.AttachHandler(element, IID_ICLICKSINK, sink);
    }
}

} // namespace

AboutDialog::AboutDialog(ISciterUI & sciterUI) :
    m_sciterUI(sciterUI),
    m_window(nullptr)
{
}

AboutDialog::~AboutDialog() = default;

void AboutDialog::Display(void * parentWindow)
{
    enum
    {
        WINDOW_WIDTH = 480,
    };

    m_window = nullptr;
    if (!m_sciterUI.WindowCreate(parentWindow, "about_dialog.html", 0, 0, WINDOW_WIDTH, 0, SUIW_CHILD, m_window))
    {
        return;
    }

    m_window->OnDestroySinkAdd(this);

    SciterElement root(m_window->GetRootElement());
    if (root.IsValid())
    {
        SciterElement versionEl(root.GetElementByID("AboutVersion"));
        if (versionEl.IsValid())
        {
            versionEl.SetText(VER_FILE_VERSION_STR);
        }

        SciterElement copyrightEl(root.GetElementByID("AboutCopyright"));
        if (copyrightEl.IsValid())
        {
            copyrightEl.SetText(VER_COPYRIGHT_STR);
        }

        AttachClickHandler(m_sciterUI, root.FindFirst("button[role=\"window-ok\"]"), this);
        AttachClickHandler(m_sciterUI, root.FindFirst("window-button[role=\"window-close\"]"), this);
        AttachClickHandler(m_sciterUI, root.GetElementByID("AboutLinkWebsite"), this);
        AttachClickHandler(m_sciterUI, root.GetElementByID("AboutLinkSource"), this);
        AttachClickHandler(m_sciterUI, root.GetElementByID("AboutLinkDiscord"), this);
    }

    m_window->FixMinSize();
    m_window->CenterWindow();
}

void AboutDialog::Close()
{
    if (m_window == nullptr || m_window->IsClosed())
    {
        return;
    }
    m_window->Destroy();
}

bool AboutDialog::OnClick(SCITER_ELEMENT element, SCITER_ELEMENT /*source*/, uint32_t /*reason*/)
{
    SciterElement clickElem(element);
    const std::string role = clickElem.GetAttribute("role");

    if (role == "window-ok" || role == "window-close")
    {
        Close();
        return true;
    }

    const std::string href = clickElem.GetAttribute("href");
    if (!href.empty() && (href.compare(0, 7, "http://") == 0 || href.compare(0, 8, "https://") == 0))
    {
        const void * owner = m_window != nullptr ? m_window->GetHandle() : nullptr;
        ShellOpen(href.c_str(), owner);
        return true;
    }

    return false;
}

void AboutDialog::OnWindowDestroy(HWINDOW /*hWnd*/)
{
    m_window = nullptr;
}
