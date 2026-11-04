// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_audio_core/audio_manager.h"
#include "core/core.h"
#include "core/hle/service/audio/errors.h"

namespace AudioCore {

AudioManager::AudioManager() {
    // Registration may happen immediately after construction, before the worker runs.
    running = true;
    thread = std::jthread([this]() { ThreadFunc(); });
}

void AudioManager::Shutdown() {
    running = false;
    events.SetAudioEvent(Event::Type::Max, true);
    thread.join();
}

Result AudioManager::SetOutManager(BufferEventFunc buffer_func, const void* owner) {
    if (!running) {
        return Service::Audio::ResultOperationFailed;
    }

    std::scoped_lock l{lock};

    const auto index{events.GetManagerIndex(Event::Type::AudioOutManager)};
    buffer_event_owners[index] = owner;
    buffer_events[index] = std::move(buffer_func);
    needs_update = true;
    events.SetAudioEvent(Event::Type::AudioOutManager, true);
    return ResultSuccess;
}

Result AudioManager::SetInManager(BufferEventFunc buffer_func, const void* owner) {
    if (!running) {
        return Service::Audio::ResultOperationFailed;
    }

    std::scoped_lock l{lock};

    const auto index{events.GetManagerIndex(Event::Type::AudioInManager)};
    buffer_event_owners[index] = owner;
    buffer_events[index] = std::move(buffer_func);
    needs_update = true;
    events.SetAudioEvent(Event::Type::AudioInManager, true);
    return ResultSuccess;
}

void AudioManager::ClearInManager(const void* owner) {
    std::scoped_lock l{lock};
    const auto index{events.GetManagerIndex(Event::Type::AudioInManager)};
    if (buffer_event_owners[index] == owner) {
        buffer_events[index] = {};
        buffer_event_owners[index] = nullptr;
    }
}

void AudioManager::ClearOutManager(const void* owner) {
    std::scoped_lock l{lock};
    const auto index{events.GetManagerIndex(Event::Type::AudioOutManager)};
    if (buffer_event_owners[index] == owner) {
        buffer_events[index] = {};
        buffer_event_owners[index] = nullptr;
    }
}

void AudioManager::SetEvent(const Event::Type type, const bool signalled) {
    events.SetAudioEvent(type, signalled);
}

void AudioManager::ThreadFunc() {
    std::unique_lock l{events.GetAudioEventLock()};
    events.ClearEvents();

    while (running) {
        const auto timed_out{events.Wait(l, std::chrono::seconds(2))};

        if (events.CheckAudioEventSet(Event::Type::Max)) {
            break;
        }

        for (size_t i = 0; i < buffer_events.size(); i++) {
            const auto event_type = static_cast<Event::Type>(i);

            if (events.CheckAudioEventSet(event_type) || timed_out) {
                BufferEventFunc callback;
                {
                    // Registration/teardown can happen on a service thread.
                    std::scoped_lock callback_lock{lock};
                    callback = buffer_events[i];
                }
                // Never hold the callback lock while taking a session manager's mutex.
                if (callback) {
                    callback();
                }
            }
            events.SetAudioEvent(event_type, false);
        }
    }
}

} // namespace AudioCore
