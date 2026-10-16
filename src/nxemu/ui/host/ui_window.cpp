#include "ui_window.h"
#include <RmlUi/Core.h>
#include <RmlUi_Platform_SDL.h>
#include <SDL_syswm.h>
#include <nxemu-module-spec/video.h>

namespace
{
using UiSystemInterface = SystemInterface_SDL;

bool QueryWindow(SDL_Window * window, SDL_SysWMinfo & info)
{
    SDL_VERSION(&info.version);
    return window != nullptr && SDL_GetWindowWMInfo(window, &info) == SDL_TRUE;
}
} // namespace

UiWindow::UiWindow() = default;

UiWindow::~UiWindow()
{
    DetachVideo();
    if (m_rmlInitialised)
    {
        Rml::Shutdown();
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
    }
    m_renderer.reset();
    m_system.reset();
    if (m_window != nullptr)
    {
        SDL_DestroyWindow(m_window);
    }
    if (m_sdlInitialised)
    {
        SDL_Quit();
    }
}

bool UiWindow::Create(const char * title, int width, int height, const std::filesystem::path & assetDir)
{
    SDL_SetMainReady();
#if defined(_WIN32)
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    // Keep what the window manager does for a framed window: snapping, the
    // shadow, the animations.
    SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");
#elif !defined(__APPLE__)
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    {
        return Fail(SDL_GetError());
    }
    m_sdlInitialised = true;

    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#else
    flags |= SDL_WINDOW_BORDERLESS;
    m_frameless = true;
#endif
    m_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (m_window == nullptr)
    {
        return Fail(SDL_GetError());
    }
    SDL_SetWindowMinimumSize(m_window, 400, 312);

#if defined(__APPLE__)
    // The layer of this view is what the video module presents to.
    m_metalView = SDL_Metal_CreateView(m_window);
    if (m_metalView == nullptr)
    {
        return Fail(SDL_GetError());
    }
#endif

    m_system = std::make_unique<UiSystemInterface>(m_window);
    m_renderer = std::make_unique<UiRenderer>();
    Rml::SetSystemInterface(m_system.get());
    Rml::SetRenderInterface(m_renderer.get());
    if (!Rml::Initialise())
    {
        return Fail("RmlUi initialization failed");
    }
    m_rmlInitialised = true;
    // Fonts are loaded before any document, which shows no text without them.
    for (const char * font : {"LatoLatin-Regular.ttf", "LatoLatin-Bold.ttf"})
    {
        if (!Rml::LoadFontFace((assetDir / "fonts" / font).string()))
        {
            return Fail(std::string("Unable to load the font ") + font);
        }
    }
    m_context = Rml::CreateContext("nxemu", {width, height});
    if (m_context == nullptr)
    {
        return Fail("Unable to create the RmlUi context");
    }
    UpdateDimensions();
    return true;
}

bool UiWindow::AttachVideo(IVideo & video)
{
    DetachVideo();
    VideoInterfaceDevice device{};
    if (!video.AttachInterface(&device))
    {
        return Fail("The video module cannot show the interface. Its Vulkan renderer is required.");
    }
    VulkanContext context;
    context.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(device.getInstanceProcAddr);
    context.instance = static_cast<VkInstance>(device.instance);
    context.physicalDevice = static_cast<VkPhysicalDevice>(device.physicalDevice);
    context.device = static_cast<VkDevice>(device.device);
    if (!m_renderer->Bind(context))
    {
        return Fail(m_renderer->Error());
    }
    m_video = &video;
    return true;
}

void UiWindow::DetachVideo()
{
    if (m_video == nullptr)
    {
        return;
    }
    m_video->WaitInterfaceIdle();
    Rml::ReleaseTextures();
    Rml::ReleaseCompiledGeometry();
    m_renderer->Unbind();
    m_video = nullptr;
}

void UiWindow::HandleEvent(const SDL_Event & event)
{
    if (event.type == SDL_WINDOWEVENT)
    {
        switch (event.window.event)
        {
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_DISPLAY_CHANGED:
        case SDL_WINDOWEVENT_MOVED:
        case SDL_WINDOWEVENT_MAXIMIZED:
        case SDL_WINDOWEVENT_RESTORED:
            UpdateDimensions();
            break;
        case SDL_WINDOWEVENT_LEAVE:
            m_context->ProcessMouseLeave();
            break;
        }
        return;
    }

    SDL_Event translated = event;
    if (translated.type == SDL_MOUSEMOTION)
    {
        translated.motion.x = static_cast<Sint32>(translated.motion.x * m_pixelScale);
        translated.motion.y = static_cast<Sint32>(translated.motion.y * m_pixelScale);
    }
    RmlSDL::InputEventHandler(m_context, m_window, translated);
}

void UiWindow::Render(const PixelRect * gameArea)
{
    const bool hidden = (SDL_GetWindowFlags(m_window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
    VideoInterfaceFrame frame{};
    if (m_video == nullptr || hidden || m_drawableWidth <= 0 || m_drawableHeight <= 0 ||
        !m_video->BeginInterfaceFrame(static_cast<uint32_t>(m_drawableWidth), static_cast<uint32_t>(m_drawableHeight), &frame))
    {
        return;
    }
    m_renderer->BeginFrame(static_cast<VkCommandBuffer>(frame.commandBuffer), m_drawableWidth, m_drawableHeight);
    m_context->Render();
    m_renderer->EndFrame(reinterpret_cast<VkImage>(frame.image));

    if (gameArea != nullptr)
    {
        const VideoInterfaceArea area{gameArea->x, gameArea->y, static_cast<uint32_t>(gameArea->width), static_cast<uint32_t>(gameArea->height)};
        m_video->EndInterfaceFrame(&area);
    }
    else
    {
        m_video->EndInterfaceFrame(nullptr);
    }
}

void UiWindow::PaceFrame()
{
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    const uint64_t elapsed = SDL_GetPerformanceCounter() - m_lastFrame;
    if (elapsed < m_frameInterval)
    {
        SDL_Delay(static_cast<Uint32>((m_frameInterval - elapsed) * 1000 / frequency));
    }
    m_lastFrame = SDL_GetPerformanceCounter();
}

void * UiWindow::RenderSurface() const
{
#if defined(__APPLE__)
    if (m_metalView != nullptr)
    {
        return SDL_Metal_GetLayer(m_metalView);
    }
#endif
    SDL_SysWMinfo info{};
    if (!QueryWindow(m_window, info))
    {
        return nullptr;
    }
    switch (info.subsystem)
    {
#if defined(SDL_VIDEO_DRIVER_WINDOWS)
    case SDL_SYSWM_WINDOWS: return info.info.win.window;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND: return info.info.wl.surface;
#endif
    default: return nullptr;
    }
}

void * UiWindow::DisplayConnection() const
{
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    SDL_SysWMinfo info{};
    if (QueryWindow(m_window, info) && info.subsystem == SDL_SYSWM_WAYLAND)
    {
        return info.info.wl.display;
    }
#endif
    return nullptr;
}

RenderWindowSystem UiWindow::WindowSystem() const
{
    SDL_SysWMinfo info{};
    if (!QueryWindow(m_window, info))
    {
        return RenderWindowSystem::Default;
    }
    switch (info.subsystem)
    {
    case SDL_SYSWM_WINDOWS: return RenderWindowSystem::Windows;
    case SDL_SYSWM_COCOA: return RenderWindowSystem::Cocoa;
    case SDL_SYSWM_WAYLAND: return RenderWindowSystem::Wayland;
    default: return RenderWindowSystem::Default;
    }
}

float UiWindow::PixelRatio() const
{
    return m_pixelScale;
}

bool UiWindow::IsFullscreen() const
{
#if defined(__APPLE__)
    return NativeWindowIsFullscreen(m_window);
#else
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
#endif
}

void UiWindow::SyncNativeState()
{
}

bool UiWindow::IsMaximised() const
{
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) != 0;
}

bool UiWindow::Fail(const std::string & message)
{
    m_error = message;
    return false;
}

void UiWindow::UpdateDimensions()
{
    SDL_DisplayMode mode{};
    const int display = std::max(SDL_GetWindowDisplayIndex(m_window), 0);
    const int rate = SDL_GetCurrentDisplayMode(display, &mode) == 0 && mode.refresh_rate > 0 ? mode.refresh_rate : 60;
    m_frameInterval = SDL_GetPerformanceFrequency() / static_cast<uint64_t>(rate);
    int width = 0, height = 0;
    SDL_GetWindowSize(m_window, &width, &height);
    SDL_GetWindowSizeInPixels(m_window, &m_drawableWidth, &m_drawableHeight);
    if (width <= 0 || m_drawableWidth <= 0)
    {
        return;
    }
    m_pixelScale = static_cast<float>(m_drawableWidth) / static_cast<float>(width);
    m_context->SetDimensions({m_drawableWidth, m_drawableHeight});

    float contentScale = m_pixelScale;
#if defined(_WIN32)
    float dpi = 0;
    if (SDL_GetDisplayDPI(std::max(SDL_GetWindowDisplayIndex(m_window), 0), nullptr, &dpi, nullptr) == 0 && dpi > 0)
    {
        contentScale = std::max(contentScale, dpi / 96.0f);
    }
#endif
    m_context->SetDensityIndependentPixelRatio(contentScale);

    m_hiddenFrame = 0;
    SDL_Rect usable{};
    if (m_frameless && IsMaximised() && !IsFullscreen() && SDL_GetDisplayUsableBounds(std::max(SDL_GetWindowDisplayIndex(m_window), 0), &usable) == 0)
    {
        int x = 0, y = 0;
        SDL_GetWindowPosition(m_window, &x, &y);
        m_hiddenFrame = static_cast<int>(static_cast<float>(std::max(usable.x - x, 0)) * m_pixelScale);
    }
}
