#include "ui_thread.h"
#include "ui_files.h"
#include "ui_renderer.h"
#include "ui_screen.h"

#include <android/log.h>
#include <RmlUi_Renderer_GL3.h>
#include <RmlUi/Core.h>
#include <thread>
namespace
{
    constexpr const char * kLogTag = "NxEmu-UI";
    constexpr auto kBusyTime = std::chrono::milliseconds(1000);
}

class UiSystem final : public Rml::SystemInterface
{
public:
private:
    const std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

UiThread & UiThread::Instance()
{
    static UiThread instance;
    return instance;
}

UiThread::UiThread() = default;
UiThread::~UiThread() = default;

void UiThread::Start(AAssetManager * assets)
{
    std::lock_guard lock(m_mutex);
    if (m_started)
    {
        return;
    }
    m_started = true;
    std::thread(&UiThread::Main, this, assets).detach();
}

void UiThread::Post(std::function<void()> task)
{
    {
        std::lock_guard lock(m_mutex);
        m_tasks.push_back(std::move(task));
        m_busyUntil = std::chrono::steady_clock::now() + kBusyTime;
    }
    m_wake.notify_one();
}

EGLSurface UiThread::CreateSurface(ANativeWindow * window)
{
    if (!m_ready || window == nullptr)
    {
        return EGL_NO_SURFACE;
    }
    EGLSurface surface = eglCreateWindowSurface(m_display, m_config, window, nullptr);
    if (surface == EGL_NO_SURFACE)
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "eglCreateWindowSurface failed: 0x%x", eglGetError());
    }
    return surface;
}

void UiThread::DestroySurface(EGLSurface surface)
{
    if (surface == EGL_NO_SURFACE)
    {
        return;
    }
    if (eglGetCurrentSurface(EGL_DRAW) == surface)
    {
        eglMakeCurrent(m_display, m_idleSurface, m_idleSurface, m_context);
    }
    eglDestroySurface(m_display, surface);
}

void UiThread::AddScreen(UiScreen * screen)
{
    m_screens.push_back(screen);
}

void UiThread::RemoveScreen(UiScreen * screen)
{
    m_screens.erase(std::remove(m_screens.begin(), m_screens.end(), screen), m_screens.end());
}

void UiThread::Main(AAssetManager * assets)
{
    m_ready = InitialiseGraphics() && InitialiseRml(assets);
    if (!m_ready)
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "The interface could not start");
    }

    for (;;)
    {
        std::deque<std::function<void()>> tasks;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return !m_tasks.empty() || WantsFrame(); });
            tasks.swap(m_tasks);
        }
        for (std::function<void()> & task : tasks)
        {
            task();
        }
        RenderScreens();
    }
}

bool UiThread::InitialiseGraphics()
{
    m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (m_display == EGL_NO_DISPLAY || !eglInitialize(m_display, nullptr, nullptr))
    {
        return false;
    }
    const EGLint configAttributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLint count = 0;
    if (!eglChooseConfig(m_display, configAttributes, &m_config, 1, &count) || count == 0)
    {
        return false;
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    m_context = eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttributes);
    if (m_context == EGL_NO_CONTEXT)
    {
        return false;
    }
    const EGLint idleAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    m_idleSurface = eglCreatePbufferSurface(m_display, m_config, idleAttributes);
    return m_idleSurface != EGL_NO_SURFACE && eglMakeCurrent(m_display, m_idleSurface, m_idleSurface, m_context);
}

bool UiThread::InitialiseRml(AAssetManager * assets)
{
    Rml::String message;
    if (!RmlGL3::Initialize(&message))
    {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", message.c_str());
        return false;
    }
    m_system = std::make_unique<UiSystem>();
    m_files = std::make_unique<UiFiles>(assets);
    m_renderer = std::make_unique<UiRenderer>();
    if (!*m_renderer)
    {
        return false;
    }
    Rml::SetSystemInterface(m_system.get());
    Rml::SetFileInterface(m_files.get());
    Rml::SetRenderInterface(m_renderer.get());
    if (!Rml::Initialise())
    {
        return false;
    }
    return true;
}

bool UiThread::WantsFrame() const
{
    const bool showing = std::any_of(m_screens.begin(), m_screens.end(), [](const UiScreen * screen) { return screen->HasSurface(); });
    if (!showing)
    {
        return false;
    }
    return std::chrono::steady_clock::now() < m_busyUntil ||
           std::any_of(m_screens.begin(), m_screens.end(), [](const UiScreen * screen) { return screen->HasSurface() && screen->Animating(); });
}

void UiThread::RenderScreens()
{
    if (!m_ready)
    {
        return;
    }
    for (UiScreen * screen : m_screens)
    {
        if (!screen->HasSurface())
        {
            continue;
        }
        const EGLSurface surface = screen->Surface();
        if (!eglMakeCurrent(m_display, surface, surface, m_context))
        {
            continue;
        }
        screen->Render(*m_renderer);
        eglSwapBuffers(m_display, surface);
    }
    eglMakeCurrent(m_display, m_idleSurface, m_idleSurface, m_context);
}
