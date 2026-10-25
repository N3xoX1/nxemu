#pragma once

#include "ui_activity.h"

#include <EGL/egl.h>
#include <memory>

class UiHost;

class UiScreen
{
public:
    UiScreen(std::unique_ptr<UiHost> host, UiActivity::Mode mode);
    ~UiScreen();

    bool Create();
    void SetWindow(ANativeWindow * window, int width, int height);
    void SetMetrics(float density, int insetLeft, int insetTop, int insetRight, int insetBottom);
    void Event(const std::string & name, const std::string & payload);

    bool HasSurface() const { return m_surface != EGL_NO_SURFACE; }
    EGLSurface Surface() const { return m_surface; }
    bool Animating() const;
    void Render(UiRenderer & renderer);

private:
    UiScreen(const UiScreen &) = delete;
    UiScreen & operator=(const UiScreen &) = delete;

    std::unique_ptr<UiHost> m_host;
    UiActivity::Mode m_mode;
    Rml::Context * m_context = nullptr;
    std::unique_ptr<UiActivity> m_activity;
    ANativeWindow * m_window = nullptr;
    EGLSurface m_surface = EGL_NO_SURFACE;
    int m_width = 0;
    int m_height = 0;
};
