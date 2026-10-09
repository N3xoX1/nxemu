#include "android_render_window.h"

#include <android/native_window.h>

AndroidRenderWindow::~AndroidRenderWindow()
{
    ClearSurface();
}

void AndroidRenderWindow::AttachSurface(ANativeWindow * native_window, float pixel_ratio)
{
    std::lock_guard lock(m_mutex);
    m_present = native_window != nullptr;
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

void AndroidRenderWindow::SuppressPresentation()
{
    std::lock_guard lock(m_mutex);
    m_present = false;
}

void AndroidRenderWindow::ClearSurface()
{
    std::lock_guard lock(m_mutex);
    m_present = false;
    if (m_native_window != nullptr)
    {
        ANativeWindow_release(m_native_window);
        m_native_window = nullptr;
    }
    m_pixel_ratio = 1.0f;
}

void * AndroidRenderWindow::DisplayConnection() const
{
    return nullptr;
}

RenderWindowSystem AndroidRenderWindow::WindowSystem() const
{
    return RenderWindowSystem::Android;
}

void * AndroidRenderWindow::RenderSurface() const
{
    std::lock_guard lock(m_mutex);
    if (!m_present)
    {
        return nullptr;
    }
    return m_native_window;
}

float AndroidRenderWindow::PixelRatio() const
{
    std::lock_guard lock(m_mutex);
    return m_pixel_ratio;
}
