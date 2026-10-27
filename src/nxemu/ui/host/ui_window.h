#pragma once
#include "ui_renderer.h"
#include <SDL.h>
#include <SDL_metal.h>
#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <nxemu-module-spec/base.h>
#include <string>

class SystemInterface_SDL;
class UiPackFileInterface;
class UiThemeFileInterface;
nxinterface IVideo;
namespace Rml
{
class Context;
}

struct PixelRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const PixelRect &) const = default;
};

class UiWindow final : public IRenderWindow
{
public:
    UiWindow();
    ~UiWindow();

    bool Create(const char * title, int width, int height, const std::filesystem::path & assetDir);
    void Show();
    bool LoadFallbackFonts(const std::function<bool()> & pump);
    const std::string & Error() const { return m_error; }

    bool AttachVideo(IVideo & video);
    void DetachVideo();

    void HandleEvent(const SDL_Event & event);
    // Returns false when nothing could be drawn.
    bool Render(const PixelRect * gameArea, bool reveal = false);
    void PaceFrame();
    void SyncNativeState();

    SDL_Window * Handle() const { return m_window; }
    void * NativeHandle() const;
    Rml::Context * Context() const { return m_context; }
    UiRenderer & Renderer() { return *m_renderer; }

    // RmlUi pixels per window coordinate unit.
    float PixelScale() const { return m_pixelScale; }

    void SetTitle(const std::string & title);
    void SetTheme(const std::string & choice);
    bool IsLightTheme() const { return m_lightTheme; }
    bool IsFullscreen() const;
    void SetFullscreen(bool fullscreen);
    bool IsMaximised() const;
    void SetMaximised(bool maximised);
    void Minimise();
    void TitleBarDoubleClicked();

    bool DrawsWindowControls() const { return m_frameless; }
    int SystemTitleBarHeight() const { return m_systemTitleBar; }
    int HiddenFrame() const { return m_hiddenFrame; }
    void GetClientSize(int & width, int & height) const;
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
    SDL_HitTestResult HitTest(const SDL_Point & point);
    bool TitleBarPressed(const SDL_Point & point);
    void ApplyTheme(bool light, bool modeChanged);
#if defined(__linux__)
    void RefreshSystemTheme();
#endif

    SDL_Window * m_window = nullptr;
    SDL_MetalView m_metalView = nullptr;
    std::unique_ptr<SystemInterface_SDL> m_system;
    std::unique_ptr<UiRenderer> m_renderer;
    std::unique_ptr<UiPackFileInterface> m_packFiles;
    std::unique_ptr<UiThemeFileInterface> m_themeFiles;
    std::string m_themeChoice = "system";
#if defined(__linux__)
    Uint32 m_themeEvent = Uint32(-1);
    Uint32 m_themeRequest = 0;
    std::future<void> m_themeQuery;
#endif
    Rml::Context * m_context = nullptr;
    IVideo * m_video = nullptr;
    int m_drawableWidth = 0;
    int m_drawableHeight = 0;
    float m_pixelScale = 1.0f;
    bool m_frameless = false;
    bool m_lightTheme = false;
    int m_systemTitleBar = 0;
    int m_hiddenFrame = 0;
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
