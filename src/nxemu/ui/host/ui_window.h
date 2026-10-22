#pragma once
#include "ui_renderer.h"
#include <SDL.h>
#include <SDL_metal.h>
#include <filesystem>
#include <memory>
#include <nxemu-module-spec/base.h>
#include <string>
#if defined(__linux__)
#include <future>
#endif

class SystemInterface_SDL;
class UiPackFileInterface;
class UiThemeFileInterface;
nxinterface IVideo;
namespace Rml
{
class Context;
}

// Rectangle in pixels of the window's drawable, origin at its top-left corner.
struct PixelRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const PixelRect &) const = default;
};

// The application window: an SDL window and the RmlUi context drawn in it.
//
// The window is also the video module's render window. The module owns the
// Vulkan device and presents to the window; the interface is drawn with that
// device and the module lays it over the game. Nothing here differs from one
// platform to the next beyond which native handle SDL hands out.
//
// The title bar belongs to the interface as well. Where the system draws its
// own buttons over the window's content (macOS) the document only leaves them
// room; elsewhere the window has no frame, and the document says what moves it
// with `data-window="drag"` and what stays clickable inside with
// `data-window="control"`.
class UiWindow final : public IRenderWindow
{
public:
    UiWindow();
    ~UiWindow();

    bool Create(const char * title, int width, int height, const std::filesystem::path & assetDir);
    const std::string & Error() const { return m_error; }

    // Draws the interface with the device of `video` from now on. Returns
    // false, with Error() set, when the module cannot show an interface.
    bool AttachVideo(IVideo & video);
    // Stops using the video module; call before it is destroyed.
    void DetachVideo();

    // Applies an SDL event to the window and to RmlUi.
    void HandleEvent(const SDL_Event & event);
    // Draws a frame and hands it to the video module, which shows the game
    // through the interface in `gameArea`, or no game when it is null.
    bool Render(const PixelRect * gameArea);
    // Paces event/input updates independently of interface redraws.
    void PaceFrame();
    void SyncNativeState();

    SDL_Window * Handle() const { return m_window; }
    // What native dialogs take as their owner (HWND, NSWindow *), or nullptr
    // where they take none.
    void * NativeHandle() const;
    Rml::Context * Context() const { return m_context; }
    UiRenderer & Renderer() { return *m_renderer; }

    // RmlUi pixels per window coordinate unit.
    float PixelScale() const { return m_pixelScale; }

    void SetTitle(const std::string & title);
    // Applies to every document without resetting its controls or content.
    void SetTheme(const std::string & choice);
    bool IsFullscreen() const;
    void SetFullscreen(bool fullscreen);
    bool IsMaximised() const;
    void SetMaximised(bool maximised);
    void Minimise();
    // What a double click on the title bar does: the system's own action
    // where it has one, maximising or restoring the window elsewhere.
    void TitleBarDoubleClicked();

    // True when the document has to draw the buttons that minimise, maximise
    // and close the window.
    bool DrawsWindowControls() const { return m_frameless; }
    // Height, in window coordinates, of the bar the system draws its buttons
    // in over the top of the document; 0 when DrawsWindowControls().
    int SystemTitleBarHeight() const { return m_systemTitleBar; }
    // Pixels around the document that fall outside the screen: a maximised
    // window without a frame is still placed as if it had one.
    int HiddenFrame() const { return m_hiddenFrame; }
    void GetClientSize(int & width, int & height) const;
    // Limited to what the screen has room for.
    void SetClientSize(int width, int height);
    void Centre();

    // IRenderWindow
    void * RenderSurface() const override;
    void * DisplayConnection() const override;
    RenderWindowSystem WindowSystem() const override;
    float PixelRatio() const override;

private:
    UiWindow(const UiWindow &) = delete;
    UiWindow & operator=(const UiWindow &) = delete;

    bool Fail(const std::string & message);
    bool LoadFonts(const std::filesystem::path & fontDir);
    void SetWindowIcon(const std::filesystem::path & iconFile);
    void UpdateDimensions();
    void ApplyTheme(bool light, bool modeChanged);
#if defined(__linux__)
    void RefreshSystemTheme();
#endif
    SDL_HitTestResult HitTest(const SDL_Point & point);
    bool TitleBarPressed(const SDL_Point & point);

    SDL_Window * m_window = nullptr;
    SDL_MetalView m_metalView = nullptr;
    std::unique_ptr<SystemInterface_SDL> m_system;
    std::unique_ptr<UiPackFileInterface> m_packFiles;
    std::unique_ptr<UiThemeFileInterface> m_themeFiles;
    std::string m_themeChoice = "system";
#if defined(__linux__)
    Uint32 m_themeEvent = Uint32(-1);
    Uint32 m_themeRequest = 0;
    std::future<void> m_themeQuery;
#endif
    std::unique_ptr<UiRenderer> m_renderer;
    Rml::Context * m_context = nullptr;
    IVideo * m_video = nullptr;
    int m_drawableWidth = 0;
    int m_drawableHeight = 0;
    float m_pixelScale = 1.0f;
    bool m_frameless = false;
    int m_systemTitleBar = 0;
    int m_hiddenFrame = 0;
    // The last press on the title bar of a window without a frame.
    uint64_t m_barPressTime = 0;
    SDL_Point m_barPress{};
    bool m_hitTestOnPress = true;
    bool m_waylandVideo = false;
    bool m_overDragArea = false;
    bool m_leftButtonDown = false;
    SDL_Point m_dragAreaPoint{};
    uint64_t m_lastFrame = 0;
    uint64_t m_frameInterval = 0;
    bool m_sdlInitialised = false;
    bool m_rmlInitialised = false;
    std::string m_error;
};
