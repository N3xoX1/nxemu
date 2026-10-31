#pragma once
#include <SDL.h>
#include <filesystem>
#include <memory>
#include <nxemu-module-spec/base.h>
#include <string>

class UiPresenter;
class UiRenderer;
namespace Rml
{
class Context;
}

class UiDialogWindow final : public IRenderWindow
{
public:
    // sharedPresenter's device is used for drawing and must outlive this.
    UiDialogWindow(SDL_Window * owner, UiPresenter & sharedPresenter, std::filesystem::path iconFile);
    ~UiDialogWindow();

    bool Prepare();
    Rml::Context * Context() const { return m_context; }
    SDL_Window * Handle() const { return m_window; }
    bool IsShown() const { return m_shown; }
    bool Owns(const SDL_Event & event) const;

    // Present/Dismiss are counted; Update hides the window once none remain.
    // A height of 0 waits for FitHeight before showing.
    void Present(const std::string & title, int width, int height);
    void FitHeight(int height);
    void Dismiss();
    bool TakeCloseRequest();

    void HandleEvent(const SDL_Event & event);
    void Update();
    void Render();
    bool Still() const;
    void SetLight(bool light);

    // IRenderWindow
    void * RenderSurface() const override;
    void * DisplayConnection() const override;
    RenderWindowSystem WindowSystem() const override;
    float PixelRatio() const override;

private:
    UiDialogWindow(const UiDialogWindow &) = delete;
    UiDialogWindow & operator=(const UiDialogWindow &) = delete;

    void Place(int width, int height, bool resizable);
    void Reveal();
    void Hide();
    void UpdateDimensions();
    float ContentScale() const;
    void SetWindowIcon();
    void ApplyFrameTheme();

    SDL_Window * const m_owner;
    UiPresenter & m_sharedPresenter;
    const std::filesystem::path m_iconFile;
    SDL_Window * m_window = nullptr;
    std::unique_ptr<UiPresenter> m_presenter;
    std::unique_ptr<UiRenderer> m_renderer;
    Rml::Context * m_context = nullptr;
    bool m_failed = false;
    bool m_shown = false;
    bool m_fitting = false;
    int m_fitWidth = 0;
    bool m_light = false;
    int m_users = 0;
    bool m_closeRequested = false;
    int m_drawableWidth = 0;
    int m_drawableHeight = 0;
    float m_pixelScale = 1.0f;
};
