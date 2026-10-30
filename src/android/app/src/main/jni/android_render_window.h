#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

#include <nxemu-module-spec/base.h>

struct ANativeWindow;

class AndroidRenderWindow final : public IRenderWindow
{
public:
    ~AndroidRenderWindow();

    void AttachSurface(ANativeWindow * native_window, float pixel_ratio);
    void SuppressPresentation();
    void ClearSurface();
    void SetPaused(bool paused);

    // IRenderWindow
    void * RenderSurface() const override;
    void * DisplayConnection() const override;
    RenderWindowSystem WindowSystem() const override;
    float PixelRatio() const override;
    void OnFramePresented(void * surface, uint32_t frame_interval_ns) override;

private:
    void ResetFrameRateHintLocked();

    mutable std::mutex m_mutex;
    ANativeWindow * m_native_window = nullptr;
    float m_pixel_ratio = 1.0f;
    bool m_present = false;
    bool m_paused = false;
    float m_last_hint = 0.f;
    std::chrono::steady_clock::time_point m_last_hint_update{};
};
