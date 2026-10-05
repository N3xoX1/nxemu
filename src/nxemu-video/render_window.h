#pragma once

#include "yuzu_video_core/frontend/emu_window.h"
#include <nxemu-module-spec/base.h>

nxinterface IRenderWindow;

class RenderWindow :
    public Core::Frontend::EmuWindow
{
public:
    RenderWindow(IRenderWindow & renderWindow);

    void RefreshRenderSurface();

    // EmuWindow
    void OnFrameDisplayed();
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const;
    bool IsShown() const;

private:
    IRenderWindow & m_renderWindow;
    bool m_firstFrame;
};