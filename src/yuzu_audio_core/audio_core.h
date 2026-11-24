// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "yuzu_audio_core/adsp/adsp.h"
#include "yuzu_audio_core/audio_manager.h"
#include "yuzu_audio_core/output_status.h"
#include "yuzu_audio_core/sink/sink.h"
#include "yuzu_common/polyfill_thread.h"

namespace Core {
class System;
}

namespace AudioCore {

class AudioManager;

/**
 * Functions called, off any emulated thread, when the layout of the output changes.
 */
class OutputListeners {
public:
    u64 Add(std::function<void()> listener);
    void Remove(u64 id);
    void Notify();

private:
    std::mutex mutex;
    u64 next_id{};
    std::vector<std::pair<u64, std::function<void()>>> listeners;
};

/**
 * Main audio class, stored inside the core, and holding the audio manager, all sinks, and the ADSP.
 */
class AudioCore {
public:
    explicit AudioCore(Core::System& system);
    ~AudioCore();

    /**
     * Shutdown the audio core.
     */
    void Shutdown();

    /**
     * Get a reference to the audio manager.
     *
     * @return Ref to the audio manager.
     */
    AudioManager& GetAudioManager();

    /// Weak handle for callbacks which may outlive the owning HLE service.
    std::weak_ptr<AudioManager> GetAudioManagerWeak() const;

    /**
     * Get the audio output sink currently in use.
     *
     * @return Ref to the sink.
     */
    Sink::Sink& GetOutputSink();

    /**
     * Get the audio input sink currently in use.
     *
     * @return Ref to the sink.
     */
    Sink::Sink& GetInputSink();

    /**
     * Get the ADSP.
     *
     * @return Ref to the ADSP.
     */
    ADSP::ADSP& ADSP();

    /// Listeners told when the output layout games see changes. Outlives services safely.
    std::weak_ptr<OutputListeners> GetOutputListeners() const;

private:
    /**
     * Keep the playback layout in line with the host output and the user's setting.
     */
    void MonitorOutput(std::stop_token stop_token);

    /**
     * Create the sinks on startup.
     */
    void CreateSinks();

    /// Main audio manager for audio in/out
    std::shared_ptr<AudioManager> audio_manager;
    /// Sink used for audio renderer and audio out
    std::unique_ptr<Sink::Sink> output_sink;
    /// Sink used for audio input
    std::unique_ptr<Sink::Sink> input_sink;
    /// The ADSP in the sysmodule
    std::unique_ptr<ADSP::ADSP> adsp;
    /// Told when the output layout changes
    std::shared_ptr<OutputListeners> output_listeners;
    /// Checks the output about once a second, and the player's choice ten times a second
    std::jthread output_monitor;
};

} // namespace AudioCore
