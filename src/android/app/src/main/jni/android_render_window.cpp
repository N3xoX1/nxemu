// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Native frame-rate API resolution adapted for NXEmu on 2026-10-30.

#include "android_render_window.h"

#include <android/native_window.h>
#include <dlfcn.h>

namespace {

// Resolve newer NDK entry points at runtime to keep API 28 loadable.
bool HintAndroidFrameRate(ANativeWindow * window, float frame_rate)
{
    using SetWithStrategy = int32_t (*)(ANativeWindow *, float, int8_t, int8_t);
    using SetFrameRate = int32_t (*)(ANativeWindow *, float, int8_t);
    static const auto with_strategy = reinterpret_cast<SetWithStrategy>(
        dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRateWithChangeStrategy"));
    static const auto set_rate = reinterpret_cast<SetFrameRate>(
        dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
    // DEFAULT compatibility, ONLY_IF_SEAMLESS: a hint, never a frame limiter.
    if (with_strategy)
    {
        return with_strategy(window, frame_rate, 0, 0) == 0;
    }
    return set_rate && set_rate(window, frame_rate, 0) == 0;
}

} // namespace

AndroidRenderWindow::~AndroidRenderWindow()
{
    ClearSurface();
}

void AndroidRenderWindow::AttachSurface(ANativeWindow * native_window, float pixel_ratio)
{
    std::lock_guard lock(m_mutex);
    m_present = native_window != nullptr;
    m_pixel_ratio = pixel_ratio > 0.f ? pixel_ratio : 1.f;
    if (m_native_window == native_window)
    {
        return;
    }
    if (m_native_window != nullptr)
    {
        ResetFrameRateHintLocked();
        ANativeWindow_release(m_native_window);
        m_native_window = nullptr;
    }
    m_native_window = native_window;
}

void AndroidRenderWindow::SuppressPresentation()
{
    std::lock_guard lock(m_mutex);
    m_present = false;
    ResetFrameRateHintLocked();
}

void AndroidRenderWindow::ClearSurface()
{
    std::lock_guard lock(m_mutex);
    m_present = false;
    if (m_native_window != nullptr)
    {
        ResetFrameRateHintLocked();
        ANativeWindow_release(m_native_window);
        m_native_window = nullptr;
    }
    m_pixel_ratio = 1.0f;
}

void AndroidRenderWindow::SetPaused(bool paused)
{
    std::lock_guard lock(m_mutex);
    m_paused = paused;
    if (paused)
    {
        ResetFrameRateHintLocked();
    }
}

void AndroidRenderWindow::ResetFrameRateHintLocked()
{
    if (m_native_window && m_last_hint > 0.f)
    {
        HintAndroidFrameRate(m_native_window, 0.f);
    }
    m_last_hint = 0.f;
    m_last_hint_update = {};
}

void AndroidRenderWindow::OnFramePresented(void * surface, uint32_t frame_interval_ns)
{
    std::lock_guard lock(m_mutex);
    if (!m_present || m_paused || !m_native_window || surface != m_native_window)
    {
        return;
    }
    // Guest VI intent follows the actual queued frame, even if the host runs slowly.
    // Zero denotes unlocked/unknown cadence: return refresh selection to Android.
    if (frame_interval_ns == 0)
    {
        ResetFrameRateHintLocked();
        return;
    }
    const float rate = 1'000'000'000.f / static_cast<float>(frame_interval_ns);
    if (rate == m_last_hint)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_last_hint_update != std::chrono::steady_clock::time_point{} &&
        now - m_last_hint_update < std::chrono::seconds{5})
    {
        return;
    }
    // Rejected/unsupported hints also obey the retry limit.
    m_last_hint_update = now;
    if (HintAndroidFrameRate(m_native_window, rate))
    {
        m_last_hint = rate;
    }
}

void * AndroidRenderWindow::DisplayConnection() const
{
    return nullptr;
}

RenderWindowSystem AndroidRenderWindow::WindowSystem() const
{
    return RenderWindowSystem::Android;
}

void * AndroidRenderWindow::RenderSurface() const
{
    std::lock_guard lock(m_mutex);
    if (!m_present)
    {
        return nullptr;
    }
    return m_native_window;
}

float AndroidRenderWindow::PixelRatio() const
{
    std::lock_guard lock(m_mutex);
    return m_pixel_ratio;
}
