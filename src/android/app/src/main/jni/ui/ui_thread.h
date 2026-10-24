#pragma once

#include <EGL/egl.h>
#include <deque>
#include <mutex>
#include <vector>

struct AAssetManager;
class UiFiles;
class UiRenderer;
class UiScreen;
class UiSystem;

class UiThread
{
public:
    static UiThread & Instance();

    void Start(AAssetManager * assets);

    void Post(std::function<void()> task);

    // These are only called on the thread.
    bool Ready() const { return m_ready; }
    UiRenderer & Renderer() { return *m_renderer; }
    UiFiles & Files() { return *m_files; }
    EGLSurface CreateSurface(ANativeWindow * window);
    void DestroySurface(EGLSurface surface);
    void AddScreen(UiScreen * screen);
    void RemoveScreen(UiScreen * screen);

private:
    UiThread();
    ~UiThread();
    UiThread(const UiThread &) = delete;
    UiThread & operator=(const UiThread &) = delete;

    void Main(AAssetManager * assets);
    bool InitialiseGraphics();
    bool InitialiseRml(AAssetManager * assets);
    bool WantsFrame() const;
    void RenderScreens();

    bool m_started = false;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::function<void()>> m_tasks;
    std::chrono::steady_clock::time_point m_busyUntil{};

    std::vector<UiScreen *> m_screens;
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLConfig m_config = nullptr;
    EGLContext m_context = EGL_NO_CONTEXT;
    EGLSurface m_idleSurface = EGL_NO_SURFACE;
    std::unique_ptr<UiSystem> m_system;
    std::unique_ptr<UiFiles> m_files;
    std::unique_ptr<UiRenderer> m_renderer;
    bool m_ready = false;
};
