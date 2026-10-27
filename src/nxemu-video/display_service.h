#pragma once

#include "render_window.h"
#include "yuzu_video_core/video_core.h"
#include <nxemu-module-spec/video.h>
#include <memory>

// Host display, not guest GPU state.  The application owns the video module
// across guest sessions, and the video module owns this service for exactly
// that lifetime.  Do not move it to the guest renderer or Host1x.
class DisplayService final
{
public:
    explicit DisplayService(IRenderWindow & hostWindow);
    ~DisplayService();

    DisplayService(const DisplayService &) = delete;
    DisplayService & operator=(const DisplayService &) = delete;

    bool Initialize();
    void Shutdown();
    RenderWindow & Window();
    Vulkan::Display * EnsureDisplay();
    void StartingGuest();
    void Resize(uint32_t width, uint32_t height);
    void RefreshWindow();
    void WaitInterfaceIdle();
    bool AttachInterface(VideoInterfaceDevice * device);
    bool BeginInterfaceFrame(uint32_t width, uint32_t height, VideoInterfaceFrame * frame);
    void EndInterfaceFrame(const VideoInterfaceArea * gameArea);

private:
    IRenderWindow & m_hostWindow;
    // Explicit destruction order: Vulkan display first, then the window
    // it creates a surface from.  The RmlUI Vulkan binding must be detached
    // before the video module is destroyed.
    std::unique_ptr<RenderWindow> m_window;
    VideoCore::DisplayPtr m_display;
};
