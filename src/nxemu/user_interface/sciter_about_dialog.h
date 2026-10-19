#pragma once

#include <sciter_handler.h>
#include <sciter_ui.h>

class SciterAboutDialog :
    public IClickSink,
    public IWindowDestroySink
{
public:
    explicit SciterAboutDialog(ISciterUI & sciterUI);
    ~SciterAboutDialog();

    void Display(void * parentWindow);

    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;
    void OnWindowDestroy(HWINDOW hWnd) override;

private:
    SciterAboutDialog(const SciterAboutDialog &) = delete;
    SciterAboutDialog & operator=(const SciterAboutDialog &) = delete;

    void Close();

    ISciterUI & m_sciterUI;
    ISciterWindow * m_window;
};
