#include "android_render_window.h"

#include <android/native_window.h>

AndroidRenderWindow::~AndroidRenderWindow()
{
    ClearSurface();
}

void AndroidRenderWindow::AttachSurface(ANativeWindow * native_window, float pixel_ratio)
{
    std::lock_guard lock(m_mutex);
    m_pixel_ratio = pixel_ratio > 0.f ? pixel_ratio : 1.f;
    if (m_native_window == native_window)
    {
        return;
    }
    if (m_native_window != nullptr)
    {
        ANativeWindow_release(m_native_window);
        m_native_window = nullptr;
    }
    m_native_window = native_window;
}

void AndroidRenderWindow::ClearSurface()
{
    std::lock_guard lock(m_mutex);
    if (m_native_window != nullptr)
    {
        ANativeWindow_release(m_native_window);
        m_native_window = nullptr;
    }
    m_pixel_ratio = 1.0f;
}

void * AndroidRenderWindow::RenderSurface() const
{
    std::lock_guard lock(m_mutex);
    return m_native_window;
}

float AndroidRenderWindow::PixelRatio() const
{
    std::lock_guard lock(m_mutex);
    return m_pixel_ratio;
}
