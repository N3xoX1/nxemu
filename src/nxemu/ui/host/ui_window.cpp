#include "ui_window.h"
#include "system_fonts.h"
#include "ui_theme.h"
#include <RmlUi/Core.h>
#include <RmlUi_Platform_SDL.h>
#include <SDL_syswm.h>
// System SDL can expose Xlib macros even when this frontend uses Wayland.
#undef None
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <nxemu-module-spec/video.h>
#include <nxemu/ui/i18n/translation.h>
#include <nxemu/ui/platform/native_window.h>
#include <stb_image.h>
#include <vector>
#include <yuzu_common/fs/fs_util.h>

namespace
{
// Width, in window coordinates, of the edges a window without a frame is
// resized by.
constexpr int kResizeEdge = 6;
#if defined(__linux__)
// Two presses on the title bar count as a double click within this time and
// this distance, in window coordinates, of each other.
constexpr uint64_t kDoubleClickMs = 500;
constexpr int kDoubleClickReach = 4;
#endif

bool QueryWindow(SDL_Window * window, SDL_SysWMinfo & info)
{
    SDL_VERSION(&info.version);
    return window != nullptr && SDL_GetWindowWMInfo(window, &info) == SDL_TRUE;
}

using UiSystemInterface = SystemInterface_SDL;
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
    if (m_metalView != nullptr)
    {
        SDL_Metal_DestroyView(m_metalView);
    }
    if (m_window != nullptr)
    {
        ReleaseNativeWindowStyle(m_window);
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
    SetWindowIcon(assetDir / "images" / "window-icon.png");
    if (m_frameless)
    {
        SDL_version linked{};
        SDL_GetVersion(&linked);
        m_hitTestOnPress = SDL_VERSIONNUM(linked.major, linked.minor, linked.patch) < SDL_VERSIONNUM(2, 32, 50);
        const char * driver = SDL_GetCurrentVideoDriver();
        m_waylandVideo = driver != nullptr && std::strcmp(driver, "wayland") == 0;
        SDL_SetWindowHitTest(
            m_window, [](SDL_Window *, const SDL_Point * point, void * self) { return static_cast<UiWindow *>(self)->HitTest(*point); }, this);
    }
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
    m_themeFiles = std::make_unique<UiThemeFileInterface>(*Rml::GetFileInterface());
    Rml::SetFileInterface(m_themeFiles.get());
    if (!LoadFonts(assetDir / "fonts"))
    {
        return false;
    }

    m_context = Rml::CreateContext("nxemu", {width, height});
    if (m_context == nullptr)
    {
        return Fail("Unable to create the RmlUi context");
    }

    m_systemTitleBar = ApplyNativeWindowStyle(m_window);
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
#if defined(__linux__)
        case SDL_WINDOWEVENT_HIT_TEST:
            if (!m_hitTestOnPress && !m_waylandVideo && m_overDragArea)
            {
                TitleBarPressed(m_dragAreaPoint);
            }
            break;
#endif
        }
        return;
    }

#if defined(__linux__)
    if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) && event.button.button == SDL_BUTTON_LEFT)
    {
        if (event.type == SDL_MOUSEBUTTONUP && !m_leftButtonDown && !m_hitTestOnPress && m_waylandVideo && m_overDragArea)
        {
            TitleBarPressed(m_dragAreaPoint);
        }
        m_leftButtonDown = event.type == SDL_MOUSEBUTTONDOWN;
    }
#endif

    SDL_Event translated = event;
    if (translated.type == SDL_MOUSEMOTION)
    {
        translated.motion.x = static_cast<Sint32>(translated.motion.x * m_pixelScale);
        translated.motion.y = static_cast<Sint32>(translated.motion.y * m_pixelScale);
    }
    RmlSDL::InputEventHandler(m_context, m_window, translated);
}

bool UiWindow::Render(const PixelRect * gameArea)
{
    const bool hidden = (SDL_GetWindowFlags(m_window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
    VideoInterfaceFrame frame{};
    if (m_video == nullptr || hidden || m_drawableWidth <= 0 || m_drawableHeight <= 0 ||
        !m_video->BeginInterfaceFrame(static_cast<uint32_t>(m_drawableWidth), static_cast<uint32_t>(m_drawableHeight), &frame))
    {
        return false;
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
    return true;
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

void * UiWindow::NativeHandle() const
{
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
#if defined(SDL_VIDEO_DRIVER_COCOA)
    case SDL_SYSWM_COCOA: return info.info.cocoa.window;
#endif
    default: return nullptr;
    }
}

void * UiWindow::RenderSurface() const
{
    if (m_metalView != nullptr)
    {
        return SDL_Metal_GetLayer(m_metalView);
    }
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

void UiWindow::SetTitle(const std::string & title)
{
    SDL_SetWindowTitle(m_window, title.c_str());
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
    if (SyncNativeWindowState(m_window))
    {
        UpdateDimensions();
    }
}

void UiWindow::SetFullscreen(bool fullscreen)
{
#if defined(__APPLE__)
    SetNativeWindowFullscreen(m_window, fullscreen);
#else
    SDL_SetWindowFullscreen(m_window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
#endif
}

bool UiWindow::IsMaximised() const
{
    return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) != 0;
}

void UiWindow::SetMaximised(bool maximised)
{
    if (maximised)
    {
        SDL_MaximizeWindow(m_window);
    }
    else
    {
        SDL_RestoreWindow(m_window);
    }
}

void UiWindow::Minimise()
{
    SDL_MinimizeWindow(m_window);
}

void UiWindow::TitleBarDoubleClicked()
{
    if (IsFullscreen())
    {
        return;
    }
#if defined(__APPLE__)
    PerformNativeTitleBarDoubleClick(m_window);
#else
    SetMaximised(!IsMaximised());
#endif
}

void UiWindow::GetClientSize(int & width, int & height) const
{
    SDL_GetWindowSize(m_window, &width, &height);
}

void UiWindow::SetClientSize(int width, int height)
{
    SDL_Rect usable{};
    const bool bounded = SDL_GetDisplayUsableBounds(std::max(SDL_GetWindowDisplayIndex(m_window), 0), &usable) == 0;
    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(m_window, &top, &left, &bottom, &right);
    if (bounded)
    {
        width = std::min(width, usable.w - left - right);
        height = std::min(height, usable.h - top - bottom);
    }
    SDL_SetWindowSize(m_window, width, height);
    if (bounded)
    {
        int x = 0, y = 0;
        SDL_GetWindowPosition(m_window, &x, &y);
        const int fitX = std::max(std::min(x, usable.x + usable.w - right - width), usable.x + left);
        const int fitY = std::max(std::min(y, usable.y + usable.h - bottom - height), usable.y + top);
        if (fitX != x || fitY != y)
        {
            SDL_SetWindowPosition(m_window, fitX, fitY);
        }
    }
}

void UiWindow::Centre()
{
    SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

bool UiWindow::Fail(const std::string & message)
{
    m_error = message;
    return false;
}

bool UiWindow::LoadFonts(const std::filesystem::path & fontDir)
{
    for (const char * font : {"LatoLatin-Regular.ttf", "LatoLatin-Bold.ttf"})
    {
        if (!Rml::LoadFontFace(Common::FS::PathToUTF8String(fontDir / font)))
        {
            return Fail(std::string("Unable to load font ") + font);
        }
    }
    for (const SystemFont & font : SystemFallbackFonts())
    {
        Rml::LoadFontFace(Common::FS::PathToUTF8String(font.path), true, Rml::Style::FontWeight::Normal, font.faceIndex);
    }
    return true;
}

void UiWindow::SetWindowIcon(const std::filesystem::path & iconFile)
{
    std::ifstream file(iconFile, std::ios::binary);
    const std::vector<char> encoded{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    int width = 0, height = 0, channels = 0;
    stbi_uc * pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(encoded.data()), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        return;
    }
    SDL_Surface * icon = SDL_CreateRGBSurfaceWithFormatFrom(pixels, width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
    if (icon != nullptr)
    {
        SDL_SetWindowIcon(m_window, icon);
        SDL_FreeSurface(icon);
    }
    stbi_image_free(pixels);
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

SDL_HitTestResult UiWindow::HitTest(const SDL_Point & point)
{
    m_overDragArea = false;
    if (IsFullscreen())
    {
        return SDL_HITTEST_NORMAL;
    }
    if (!IsMaximised())
    {
        int width = 0, height = 0;
        SDL_GetWindowSize(m_window, &width, &height);
        const int column = point.x < kResizeEdge ? 0 : point.x >= width - kResizeEdge ? 2 : 1;
        const int row = point.y < kResizeEdge ? 0 : point.y >= height - kResizeEdge ? 2 : 1;
        static const SDL_HitTestResult edges[3][3] = {
            {SDL_HITTEST_RESIZE_TOPLEFT, SDL_HITTEST_RESIZE_TOP, SDL_HITTEST_RESIZE_TOPRIGHT},
            {SDL_HITTEST_RESIZE_LEFT, SDL_HITTEST_NORMAL, SDL_HITTEST_RESIZE_RIGHT},
            {SDL_HITTEST_RESIZE_BOTTOMLEFT, SDL_HITTEST_RESIZE_BOTTOM, SDL_HITTEST_RESIZE_BOTTOMRIGHT},
        };
        if (edges[row][column] != SDL_HITTEST_NORMAL)
        {
            return edges[row][column];
        }
    }
    const Rml::Vector2f position(static_cast<float>(point.x) * m_pixelScale, static_cast<float>(point.y) * m_pixelScale);
    for (Rml::Element * element = m_context->GetElementAtPoint(position); element != nullptr; element = element->GetParentNode())
    {
        if (element->HasAttribute("data-window"))
        {
            if (element->GetAttribute<Rml::String>("data-window", "") != "drag")
            {
                return SDL_HITTEST_NORMAL;
            }
#if defined(__linux__)
            if (m_hitTestOnPress && TitleBarPressed(point))
            {
                return SDL_HITTEST_NORMAL;
            }
            m_overDragArea = true;
            m_dragAreaPoint = point;
#endif
            return SDL_HITTEST_DRAGGABLE;
        }
    }
    return SDL_HITTEST_NORMAL;
}

bool UiWindow::TitleBarPressed(const SDL_Point & point)
{
#if !defined(__linux__)
    (void)point;
    return false;
#else
    const uint64_t now = SDL_GetTicks64();
    const bool again = now - m_barPressTime <= kDoubleClickMs && std::abs(point.x - m_barPress.x) <= kDoubleClickReach &&
                       std::abs(point.y - m_barPress.y) <= kDoubleClickReach;
    m_barPressTime = again ? 0 : now;
    m_barPress = point;
    if (again)
    {
        TitleBarDoubleClicked();
    }
    return again;
#endif
}
