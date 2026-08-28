#pragma once

#include <sciter_handler.h>
#include <sciter_ui.h>

class AboutDialog :
    public IClickSink,
    public IWindowDestroySink
{
public:
    explicit AboutDialog(ISciterUI & sciterUI);
    ~AboutDialog();

    void Display(void * parentWindow);

    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;
    void OnWindowDestroy(HWINDOW hWnd) override;

private:
    AboutDialog(const AboutDialog &) = delete;
    AboutDialog & operator=(const AboutDialog &) = delete;

    void Close();

    ISciterUI & m_sciterUI;
    ISciterWindow * m_window;
};
