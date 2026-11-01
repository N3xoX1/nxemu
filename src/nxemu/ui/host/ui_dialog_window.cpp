#include "ui_dialog_window.h"
#include "ui_pack.h"
#include "ui_presenter.h"
#include "ui_renderer.h"
#include <RmlUi/Core.h>
#include <RmlUi_Platform_SDL.h>
#include <algorithm>
#include <stb_image.h>
#include <yuzu_common/logging/log.h>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
constexpr const char * kContextName = "nxemu-dialog";
constexpr int kMinimumWidth = 640;
constexpr int kMinimumHeight = 420;
constexpr int kScreenMargin = 24;

void * WindowPointer(SDL_Window * window, const char * property)
{
    return window != nullptr ? SDL_GetPointerProperty(SDL_GetWindowProperties(window), property, nullptr) : nullptr;
}

#if defined(_WIN32)
HWND WindowHandle(SDL_Window * window)
{
    return static_cast<HWND>(WindowPointer(window, SDL_PROP_WINDOW_WIN32_HWND_POINTER));
}
#endif
} // namespace

UiDialogWindow::UiDialogWindow(SDL_Window * owner, UiPresenter & sharedPresenter, std::filesystem::path iconFile) :
    m_owner(owner),
    m_sharedPresenter(sharedPresenter),
    m_iconFile(std::move(iconFile))
{
}

UiDialogWindow::~UiDialogWindow()
{
    Hide();
    if (m_presenter != nullptr)
    {
        m_presenter->WaitIdle();
    }
    if (m_context != nullptr)
    {
        Rml::RemoveContext(kContextName);
        m_context = nullptr;
    }
    if (m_renderer != nullptr && m_renderer->IsBound())
    {
        Rml::ReleaseTextures(m_renderer.get());
        Rml::ReleaseCompiledGeometry(m_renderer.get());
        m_renderer->Unbind();
    }
    m_renderer.reset();
    m_presenter.reset();
    if (m_window != nullptr)
    {
        SDL_DestroyWindow(m_window);
    }
}

bool UiDialogWindow::Prepare()
{
    if (m_context != nullptr)
    {
        return true;
    }
    if (m_failed)
    {
        return false;
    }
    m_failed = true;

    m_window = SDL_CreateWindow("NXEmu", 960, 600, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (m_window == nullptr)
    {
        LOG_WARNING(Frontend, "Dialog window: {}", SDL_GetError());
        return false;
    }
#if defined(_WIN32)
    // Owned windows stay in front of the owner and out of the taskbar.
    SetWindowLongPtrW(WindowHandle(m_window), GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(WindowHandle(m_owner)));
#endif
    SetWindowIcon();

    m_presenter = std::make_unique<UiPresenter>();
    if (!m_presenter->Create(*this, m_sharedPresenter))
    {
        LOG_WARNING(Frontend, "Dialog window: {}", m_presenter->Error());
        return false;
    }
    m_renderer = std::make_unique<UiRenderer>();
    if (!m_renderer->Bind(m_presenter->Context()))
    {
        LOG_WARNING(Frontend, "Dialog window: {}", m_renderer->Error());
        return false;
    }
    m_context = Rml::CreateContext(kContextName, {960, 600}, m_renderer.get());
    if (m_context == nullptr)
    {
        LOG_WARNING(Frontend, "Dialog window: unable to create the RmlUi context");
        return false;
    }
    m_context->ActivateTheme("light", m_light);
    ApplyFrameTheme();
    UpdateDimensions();
    m_failed = false;
    return true;
}

bool UiDialogWindow::Owns(const SDL_Event & event) const
{
    return m_window != nullptr && SDL_GetWindowFromEvent(&event) == m_window;
}

void UiDialogWindow::Present(const std::string & title, int width, int height)
{
    ++m_users;
    SDL_SetWindowTitle(m_window, title.c_str());
    if (m_shown || m_fitting)
    {
        return;
    }
    if (height > 0)
    {
        Place(width, height, true);
        Reveal();
        return;
    }
    Place(width, width, false);
    m_fitting = true;
    m_fitWidth = width;
}

void UiDialogWindow::FitHeight(int height)
{
    if (!m_fitting)
    {
        return;
    }
    m_fitting = false;
    Place(m_fitWidth, height, false);
    Reveal();
}

void UiDialogWindow::Dismiss()
{
    if (m_users > 0)
    {
        --m_users;
    }
}

bool UiDialogWindow::TakeCloseRequest()
{
    const bool requested = m_closeRequested;
    m_closeRequested = false;
    return requested;
}

void UiDialogWindow::Place(int width, int height, bool resizable)
{
    SDL_DisplayID display = SDL_GetDisplayForWindow(m_owner);
    if (display == 0)
    {
        display = SDL_GetPrimaryDisplay();
    }
    const float scale = ContentScale();
    SDL_Rect usable{};
    if (!SDL_GetDisplayUsableBounds(display, &usable))
    {
        usable = {0, 0, 1280, 720};
    }
    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(m_window, &top, &left, &bottom, &right);
    const int margin = static_cast<int>(kScreenMargin * scale);
    const int roomWidth = std::max(usable.w - left - right - margin * 2, 1);
    const int roomHeight = std::max(usable.h - top - bottom - margin * 2, 1);
    const int minimumWidth = resizable ? std::min(static_cast<int>(kMinimumWidth * scale), roomWidth) : 1;
    const int minimumHeight = resizable ? std::min(static_cast<int>(kMinimumHeight * scale), roomHeight) : 1;
    width = std::clamp(static_cast<int>(width * scale), minimumWidth, roomWidth);
    height = std::clamp(static_cast<int>(height * scale), minimumHeight, roomHeight);
    SDL_SetWindowResizable(m_window, resizable);
    SDL_SetWindowMinimumSize(m_window, minimumWidth, minimumHeight);
    SDL_SetWindowSize(m_window, width, height);

    int ownerX = 0, ownerY = 0, ownerWidth = 0, ownerHeight = 0;
    SDL_GetWindowPosition(m_owner, &ownerX, &ownerY);
    SDL_GetWindowSize(m_owner, &ownerWidth, &ownerHeight);
    const int x = std::clamp(ownerX + (ownerWidth - width) / 2, usable.x + left, std::max(usable.x + usable.w - right - width, usable.x + left));
    const int y = std::clamp(ownerY + (ownerHeight - height) / 2, usable.y + top, std::max(usable.y + usable.h - bottom - height, usable.y + top));
    SDL_SetWindowPosition(m_window, x, y);
    UpdateDimensions();
}

void UiDialogWindow::Reveal()
{
    m_closeRequested = false;
    m_shown = true;
    SDL_ShowWindow(m_window);
    SDL_RaiseWindow(m_window);
#if defined(_WIN32)
    EnableWindow(WindowHandle(m_owner), FALSE);
#endif
}

void UiDialogWindow::Hide()
{
    if (!m_shown)
    {
        return;
    }
    m_shown = false;
#if defined(_WIN32)
    // Re-enabled before hiding so that Windows activates it.
    EnableWindow(WindowHandle(m_owner), TRUE);
#endif
    SDL_HideWindow(m_window);
    SDL_RaiseWindow(m_owner);
    if (m_context != nullptr)
    {
        m_context->ProcessMouseLeave();
    }
}

void UiDialogWindow::HandleEvent(const SDL_Event & event)
{
    if (m_context == nullptr)
    {
        return;
    }
    if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
    {
        switch (event.type)
        {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        case SDL_EVENT_WINDOW_MOVED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
            UpdateDimensions();
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            m_context->ProcessMouseLeave();
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            m_closeRequested = true;
            break;
        }
        return;
    }
    // The handler scales mouse positions by the window's pixel density.
    SDL_Event forwarded = event;
    RmlSDL::InputEventHandler(m_context, m_window, forwarded);
}

void UiDialogWindow::Update()
{
    if (m_users == 0)
    {
        m_fitting = false;
        Hide();
    }
    if (m_context != nullptr)
    {
        m_context->Update();
    }
}

void UiDialogWindow::Render()
{
    if (!m_shown || (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0 || m_drawableWidth <= 0 || m_drawableHeight <= 0)
    {
        return;
    }
    VkImage image = VK_NULL_HANDLE;
    const VkCommandBuffer commandBuffer = m_presenter->BeginFrame(static_cast<uint32_t>(m_drawableWidth), static_cast<uint32_t>(m_drawableHeight), image);
    if (commandBuffer == VK_NULL_HANDLE)
    {
        return;
    }
    m_renderer->BeginFrame(commandBuffer, m_drawableWidth, m_drawableHeight);
    m_context->Render();
    m_renderer->EndFrame(image);
    m_presenter->EndFrame();
}

bool UiDialogWindow::Still() const
{
    return !m_shown || m_context == nullptr || m_context->GetNextUpdateDelay() > 0;
}

void UiDialogWindow::SetLight(bool light)
{
    m_light = light;
    if (m_context == nullptr)
    {
        return;
    }
    m_context->ActivateTheme("light", light);
    for (int index = 0; index < m_context->GetNumDocuments(); ++index)
    {
        m_context->GetDocument(index)->ReloadStyleSheet();
    }
    ApplyFrameTheme();
}

void * UiDialogWindow::RenderSurface() const
{
    // Dialogs get their own window only on Windows.
    return WindowPointer(m_window, SDL_PROP_WINDOW_WIN32_HWND_POINTER);
}

void * UiDialogWindow::DisplayConnection() const
{
    return nullptr;
}

RenderWindowSystem UiDialogWindow::WindowSystem() const
{
    return WindowPointer(m_window, SDL_PROP_WINDOW_WIN32_HWND_POINTER) != nullptr ? RenderWindowSystem::Windows : RenderWindowSystem::Default;
}

float UiDialogWindow::PixelRatio() const
{
    return m_pixelScale;
}

void UiDialogWindow::UpdateDimensions()
{
    int width = 0, height = 0;
    SDL_GetWindowSize(m_window, &width, &height);
    SDL_GetWindowSizeInPixels(m_window, &m_drawableWidth, &m_drawableHeight);
    if (width <= 0 || m_drawableWidth <= 0 || m_context == nullptr)
    {
        return;
    }
    m_pixelScale = static_cast<float>(m_drawableWidth) / static_cast<float>(width);
    m_context->SetDimensions({m_drawableWidth, m_drawableHeight});
    m_context->SetDensityIndependentPixelRatio(std::max(m_pixelScale, ContentScale()));
}

float UiDialogWindow::ContentScale() const
{
    SDL_Window * window = m_shown ? m_window : m_owner;
    float scale = 1.0f;
#if defined(_WIN32)
    // Window coordinates are pixels on Windows; the display scale carries the DPI.
    const float displayScale = SDL_GetWindowDisplayScale(window);
    if (displayScale > 0.0f)
    {
        scale = displayScale;
    }
#else
    (void)window;
#endif
    return scale;
}

void UiDialogWindow::SetWindowIcon()
{
    std::string encoded;
    if (!UiPack::ReadFile(m_iconFile, encoded))
    {
        return;
    }
    int width = 0, height = 0, channels = 0;
    stbi_uc * pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(encoded.data()), static_cast<int>(encoded.size()), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        return;
    }
    SDL_Surface * icon = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4);
    if (icon != nullptr)
    {
        SDL_SetWindowIcon(m_window, icon);
        SDL_DestroySurface(icon);
    }
    stbi_image_free(pixels);
}

void UiDialogWindow::ApplyFrameTheme()
{
#if defined(_WIN32)
    using SetAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static const SetAttribute setAttribute = [] {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        return dwm != nullptr ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm, "DwmSetWindowAttribute")) : nullptr;
    }();
    HWND window = WindowHandle(m_window);
    if (setAttribute == nullptr || window == nullptr)
    {
        return;
    }
    constexpr DWORD kImmersiveDarkMode = 20;
    constexpr DWORD kImmersiveDarkModeBefore20H1 = 19;
    const BOOL dark = m_light ? FALSE : TRUE;
    if (FAILED(setAttribute(window, kImmersiveDarkMode, &dark, sizeof(dark))))
    {
        setAttribute(window, kImmersiveDarkModeBefore20H1, &dark, sizeof(dark));
    }
#endif
}
