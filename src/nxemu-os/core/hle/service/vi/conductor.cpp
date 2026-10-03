// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/vi/conductor.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/perf_stats.h"
#include "core/hle/service/vi/container.h"
#include "core/hle/service/vi/display_list.h"
#include "core/hle/service/vi/vsync_manager.h"
#include "os_settings.h"
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-video/video_settings_identifiers.h>

constexpr auto FrameNs = std::chrono::nanoseconds{1000000000 / 60};

extern IModuleSettings * g_settings;

namespace Service::VI
{

Conductor::Conductor(Core::System & system, Container & container, DisplayList & displays) :
    m_system(system), m_container(container)
{
    displays.ForEachDisplay([&](Display & display) {
        m_vsync_managers.insert({display.GetId(), VsyncManager{}});
    });

    // Keep speed limiting on the guest CPU in debug single-core mode.
    if (system.IsMulticore()) {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                m_signal.Set();
                return std::chrono::nanoseconds(this->GetNextTicks());
            });
    } else {
        m_event = Core::Timing::CreateEvent(
            "ScreenComposition",
            [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                m_signal.Set();
                m_system.SpeedLimiter().DoSpeedLimiting(m_system.CoreTiming().GetGlobalTimeUs());
                return std::chrono::nanoseconds(this->GetNextTicks());
            });
    }

    system.CoreTiming().ScheduleLoopingEvent(FrameNs, FrameNs, m_event);
    m_thread = std::jthread([this](std::stop_token token) { this->VsyncThread(token); });
}

Conductor::~Conductor()
{
    m_system.CoreTiming().UnscheduleEvent(m_event);

    m_thread.request_stop();
    m_signal.Set();
    m_thread.join();
}

void Conductor::LinkVsyncEvent(u64 display_id, Event * event)
{
    std::scoped_lock lk{m_vsync_mutex};
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end())
    {
        it->second.LinkVsyncEvent(event);
    }
}

void Conductor::UnlinkVsyncEvent(u64 display_id, Event * event)
{
    std::scoped_lock lk{m_vsync_mutex};
    if (auto it = m_vsync_managers.find(display_id); it != m_vsync_managers.end())
    {
        it->second.UnlinkVsyncEvent(event);
    }
}

void Conductor::ProcessVsync()
{
    for (auto & [display_id, manager] : m_vsync_managers)
    {
        auto swap_interval = m_swap_interval.load(std::memory_order_relaxed);
        auto compose_speed_scale = m_compose_speed_scale.load(std::memory_order_relaxed);
        m_container.ComposeOnDisplay(&swap_interval, &compose_speed_scale, display_id);
        m_swap_interval.store(swap_interval, std::memory_order_relaxed);
        m_compose_speed_scale.store(compose_speed_scale, std::memory_order_relaxed);
        std::scoped_lock lk{m_vsync_mutex};
        manager.SignalVsync();
    }
}

void Conductor::VsyncThread(std::stop_token token)
{
    Common::SetCurrentThreadName("VSyncThread");

    while (!token.stop_requested())
    {
        m_signal.Wait();

        if (token.stop_requested())
        {
            return;
        }

        if (m_system.IsShuttingDown())
        {
            return;
        }

        this->ProcessVsync();
    }
}

s64 Conductor::GetNextTicks() const
{
    auto speed_scale = 1.f;
    if (osSettings.use_multi_core)
    {
        if (osSettings.use_speed_limit)
        {
            // Scales the speed based on speed_limit setting on MC. SC is handled by
            // SpeedLimiter::DoSpeedLimiting.
            speed_scale = 100.f / osSettings.speed_limit;
        }
        else
        {
            // Run at unlocked framerate.
            speed_scale = 0.01f;
        }
    }

    // Adjust by speed limit determined during composition.
    speed_scale /= m_compose_speed_scale.load(std::memory_order_relaxed);

    if (m_system.GetNVDECActive() && g_settings->GetBool(NXVideoSetting::SyncToFramerateOfVideoPlayback))
    {
        // Run at intended presentation rate during video playback.
        speed_scale = 1.f;
    }

    const f32 effective_fps =
        60.f / static_cast<f32>(m_swap_interval.load(std::memory_order_relaxed));
    return static_cast<s64>(speed_scale * (1000000000.f / effective_fps));
}

} // namespace Service::VI
