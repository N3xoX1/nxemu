// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_audio_core/audio_core.h"

#include <atomic>

#include "core/core.h"
#include "nxemu-os/os_settings.h"
#include "yuzu_audio_core/sink/sink_details.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/thread.h"

#ifdef _WIN32
#include <objbase.h>
#endif

namespace AudioCore
{

namespace
{
bool SurroundAllowed()
{
    return osSettings.surround_allowed.load(std::memory_order_relaxed);
}

// One word, so a reader never mixes two monitor passes: a byte per field.
std::atomic<u32> published_status{};

void PublishStatus(const Sink::Sink * sink, bool surround_allowed)
{
    u32 packed = 0;
    if (sink != nullptr)
    {
        // What the streams play, which is less than asked when a device refused 5.1.
        const Sink::Sink::PlayingChannels playing = sink->GetPlayingChannels();
        packed = (playing.output & 0xFF) | (sink->GetHostChannels() & 0xFF) << 8 |
                 (playing.source & 0xFF) << 16 | (surround_allowed ? 1U : 0U) << 24;
    }
    published_status.store(packed, std::memory_order_relaxed);
}
} // namespace

OutputStatus PublishedOutputStatus()
{
    const u32 packed = published_status.load(std::memory_order_relaxed);
    return {packed & 0xFF, packed >> 8 & 0xFF, packed >> 16 & 0xFF, (packed >> 24 & 1) != 0};
}

u64 OutputListeners::Add(std::function<void()> listener)
{
    std::scoped_lock lock{mutex};
    listeners.emplace_back(++next_id, std::move(listener));
    return next_id;
}

void OutputListeners::Remove(u64 id)
{
    std::scoped_lock lock{mutex};
    std::erase_if(listeners, [id](const auto & entry) { return entry.first == id; });
}

void OutputListeners::Notify()
{
    std::scoped_lock lock{mutex};
    for (const auto & entry : listeners)
    {
        entry.second();
    }
}

AudioCore::AudioCore(Core::System & system) :
    audio_manager{std::make_shared<AudioManager>()},
    output_listeners{std::make_shared<OutputListeners>()}
{
    CreateSinks();
    // Before the renderer opens its streams, so that they start in the right layout.
    output_sink->UpdateOutput(SurroundAllowed());
    LOG_INFO(Audio, "Games get {} channels, output plays {} (host {})", output_sink->GetGuestChannels(),
             output_sink->GetDeviceChannels(), output_sink->GetHostChannels());
    // Must be created after the sinks
    adsp = std::make_unique<ADSP::ADSP>(system, *output_sink);
    output_monitor = std::jthread([this](std::stop_token stop_token) { MonitorOutput(stop_token); });
}

AudioCore ::~AudioCore()
{
    Shutdown();
}

void AudioCore::CreateSinks()
{
    output_sink = Sink::CreateSinkFromID(osSettings.sink_id, osSettings.audio_output_device_id);
    input_sink = Sink::CreateSinkFromID(osSettings.sink_id, osSettings.audio_input_device_id);
}

void AudioCore::Shutdown()
{
    output_monitor = {};
    PublishStatus(nullptr, false);
    audio_manager->Shutdown();
}

void AudioCore::MonitorOutput(std::stop_token stop_token)
{
    Common::SetCurrentThreadName("AudioOutputMonitor");
#ifdef _WIN32
    // Cubeb reads the default output through COM.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    // A device query costs microseconds; a second is short next to plugging in a device. The
    // player's choice is checked more often, so a click on the badge is heard at once.
    constexpr int ChecksPerQuery = 10;
    bool allowed = SurroundAllowed();
    int checks = 0;
    while (Common::StoppableTimedWait(stop_token, std::chrono::milliseconds{100}))
    {
        const bool now_allowed = SurroundAllowed();
        if (++checks < ChecksPerQuery && now_allowed == allowed)
        {
            continue;
        }
        checks = 0;
        allowed = now_allowed;
        const u32 playing = output_sink->GetDeviceChannels();
        const bool games_told = output_sink->UpdateOutput(allowed);
        if (games_told || playing != output_sink->GetDeviceChannels())
        {
            LOG_INFO(Audio, "Games get {} channels, output now plays {} (host {})",
                     output_sink->GetGuestChannels(), output_sink->GetDeviceChannels(),
                     output_sink->GetHostChannels());
        }
        if (games_told)
        {
            output_listeners->Notify();
        }
        PublishStatus(output_sink.get(), allowed);
    }
#ifdef _WIN32
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
#endif
}

std::weak_ptr<OutputListeners> AudioCore::GetOutputListeners() const
{
    return output_listeners;
}

AudioManager & AudioCore::GetAudioManager()
{
    return *audio_manager;
}

std::weak_ptr<AudioManager> AudioCore::GetAudioManagerWeak() const
{
    return audio_manager;
}

Sink::Sink & AudioCore::GetOutputSink()
{
    return *output_sink;
}

Sink::Sink & AudioCore::GetInputSink()
{
    return *input_sink;
}

ADSP::ADSP & AudioCore::ADSP()
{
    return *adsp;
}

} // namespace AudioCore
