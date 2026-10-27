#include "display_service.h"
#include "video_settings.h"
#include <chrono>
#include <yuzu_common/logging/log.h>

DisplayService::DisplayService(IRenderWindow & hostWindow) : m_hostWindow(hostWindow) {}

// The member declaration order destroys the Vulkan display before its window.
// Explicit Shutdown() remains available for the owner; RAII does not need to
// call it for a second time when the service itself is destroyed.
DisplayService::~DisplayService() = default;

bool DisplayService::Initialize()
{
    if (!m_window)
    {
        m_window = std::make_unique<RenderWindow>(m_hostWindow);
    }
    return true;
}

void DisplayService::Shutdown()
{
    // Must run after the guest GPU and interface have released their
    // resources.  There is no second Vulkan instance or device to destroy.
    m_display.reset();
    m_window.reset();
}

RenderWindow & DisplayService::Window()
{
    return *m_window;
}

Vulkan::Display * DisplayService::EnsureDisplay()
{
    if (m_display || !m_window || videoSettings.renderer_backend != RendererBackend::Vulkan)
    {
        return m_display.get();
    }
    const auto start = std::chrono::steady_clock::now();
    m_display = VideoCore::CreateDisplay(*m_window);
    LOG_DEBUG(Render, "Vulkan display initialization: {} ms (success={})",
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count(), m_display != nullptr);
    return m_display.get();
}

void DisplayService::StartingGuest()
{
    if (m_window)
    {
        m_window->ResetFirstFrame();
        m_window->RefreshRenderSurface();
    }
}

void DisplayService::Resize(uint32_t width, uint32_t height)
{
    if (m_window)
    {
        m_window->RefreshRenderSurface();
        m_window->UpdateCurrentFramebufferLayout(width, height);
    }
}

void DisplayService::RefreshWindow()
{
    if (m_window)
    {
        m_window->RefreshRenderSurface();
    }
}

void DisplayService::WaitInterfaceIdle()
{
    if (m_display)
    {
        VideoCore::WaitInterfaceIdle(*m_display);
    }
}

bool DisplayService::AttachInterface(VideoInterfaceDevice * device)
{
    Vulkan::Display * const display = EnsureDisplay();
    return display != nullptr && device != nullptr && VideoCore::AttachInterface(*display, *device);
}

bool DisplayService::BeginInterfaceFrame(uint32_t width, uint32_t height, VideoInterfaceFrame * frame)
{
    if (!m_display || frame == nullptr || width == 0 || height == 0)
    {
        return false;
    }
    return VideoCore::BeginInterfaceFrame(*m_display, width, height, *frame);
}

void DisplayService::EndInterfaceFrame(const VideoInterfaceArea * gameArea)
{
    if (m_display)
    {
        VideoCore::EndInterfaceFrame(*m_display, gameArea);
    }
}
