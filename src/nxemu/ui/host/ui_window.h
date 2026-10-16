#pragma once
#include "ui_renderer.h"
#include <SDL.h>
#include <filesystem>
#include <nxemu-module-spec/base.h>

class SystemInterface_SDL;

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
    const std::string & Error() const { return m_error; }

    bool AttachVideo(IVideo & video);
    void DetachVideo();

    void HandleEvent(const SDL_Event & event);
    void Render(const PixelRect * gameArea);
    void PaceFrame();
    void SyncNativeState();

    SDL_Window * Handle() const { return m_window; }
    Rml::Context * Context() const { return m_context; }

    bool IsFullscreen() const;
    bool IsMaximised() const;

    // IRenderWindow
    void * RenderSurface() const override;
    void * DisplayConnection() const override;
    RenderWindowSystem WindowSystem() const override;
    float PixelRatio() const override;

private:
    UiWindow(const UiWindow &) = delete;
    UiWindow & operator=(const UiWindow &) = delete;

    bool Fail(const std::string & message);
    void UpdateDimensions();

    SDL_Window * m_window = nullptr;
    std::unique_ptr<SystemInterface_SDL> m_system;
    std::unique_ptr<UiRenderer> m_renderer;
    Rml::Context * m_context = nullptr;
    IVideo * m_video = nullptr;
    int m_drawableWidth = 0;
    int m_drawableHeight = 0;
    float m_pixelScale = 1.0f;
    bool m_frameless = false;
    int m_hiddenFrame = 0;
    uint64_t m_lastFrame = 0;
    uint64_t m_frameInterval = 0;
    bool m_sdlInitialised = false;
    bool m_rmlInitialised = false;
    std::string m_error;
};
