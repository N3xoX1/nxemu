#include "ui_screen.h"
#include "ui_host.h"
#include "ui_renderer.h"
#include "ui_thread.h"

#include <android/native_window.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>

namespace
{
    int s_nextContext = 0;
}

UiScreen::UiScreen(std::unique_ptr<UiHost> host, UiActivity::Mode mode) :
    m_host(std::move(host)),
    m_mode(mode)
{
}

UiScreen::~UiScreen()
{
    UiThread::Instance().RemoveScreen(this);
    SetWindow(nullptr, 0, 0);
    m_activity.reset();
    if (m_context != nullptr)
    {
        Rml::RemoveContext(m_context->GetName());
    }
}

bool UiScreen::Create()
{
    UiThread & thread = UiThread::Instance();
    if (!thread.Ready())
    {
        return false;
    }
    m_context = Rml::CreateContext("screen" + std::to_string(s_nextContext++), {1, 1});
    if (m_context == nullptr)
    {
        return false;
    }
    m_activity = std::make_unique<UiActivity>(*m_context, *m_host, thread.Renderer(), m_mode);
    if (!m_activity->Load())
    {
        return false;
    }
    thread.AddScreen(this);
    return true;
}

void UiScreen::SetWindow(ANativeWindow * window, int width, int height)
{
    UiThread & thread = UiThread::Instance();
    if (window != m_window)
    {
        thread.DestroySurface(m_surface);
        m_surface = EGL_NO_SURFACE;
        if (m_window != nullptr)
        {
            ANativeWindow_release(m_window);
        }
        m_window = window;
        m_surface = thread.CreateSurface(window);
    }
    else if (window != nullptr)
    {
        ANativeWindow_release(window);
    }
    m_width = width;
    m_height = height;
    if (m_context != nullptr && width > 0 && height > 0)
    {
        m_context->SetDimensions({width, height});
        if (m_activity)
        {
            m_activity->Resized();
        }
    }
}

void UiScreen::SetMetrics(float density, int insetLeft, int insetTop, int insetRight, int insetBottom)
{
    if (m_context == nullptr)
    {
        return;
    }
    m_context->SetDensityIndependentPixelRatio(density);
    m_activity->SetInsets(insetLeft, insetTop, insetRight, insetBottom);
}

void UiScreen::Touch(int action, float x, float y)
{
    if (m_context == nullptr)
    {
        return;
    }
    const Rml::TouchList touches = {Rml::Touch{0, {x, y}}};
    switch (action)
    {
    case TouchDown:
        if (m_touching)
        {
            m_context->ProcessTouchCancel(touches);
        }
        m_touching = true;
        m_context->ProcessTouchStart(touches, 0);
        break;
    case TouchMove:
        if (m_touching)
        {
            m_context->ProcessTouchMove(touches, 0);
        }
        break;
    case TouchUp:
        if (m_touching)
        {
            m_touching = false;
            m_context->ProcessTouchEnd(touches, 0);
            m_context->ProcessMouseLeave();
        }
        break;
    case TouchCancel:
        if (m_touching)
        {
            m_touching = false;
            m_context->ProcessTouchCancel(touches);
            m_context->ProcessMouseLeave();
        }
        break;
    }
}

void UiScreen::Event(const std::string & name, const std::string & payload)
{
    if (m_activity)
    {
        m_activity->Event(name, payload);
    }
}

bool UiScreen::Animating() const
{
    return m_activity && m_activity->Animating();
}

void UiScreen::Render(UiRenderer & renderer)
{
    if (m_context == nullptr || m_width <= 0 || m_height <= 0)
    {
        return;
    }
    m_activity->Update();
    m_context->Update();
    renderer.SetViewport(m_width, m_height);
    renderer.BeginFrame();
    renderer.Clear();
    m_context->Render();
    renderer.EndFrame();
}
