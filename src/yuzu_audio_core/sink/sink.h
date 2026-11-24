// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "yuzu_audio_core/sink/sink_stream.h"
#include "yuzu_audio_core/sink/speaker_layout.h"
#include "yuzu_common/common_types.h"

namespace Common {
class Event;
}
namespace Core {
class System;
}

namespace AudioCore::Sink {

constexpr char auto_device_name[] = "auto";

/**
 * This class is an interface for an audio sink, holds multiple output streams and is responsible
 * for sinking samples to hardware. Used by Audio Render, Audio In and Audio Out.
 */
class Sink {
public:
    virtual ~Sink() = default;
    /**
     * Close a given stream.
     *
     * @param stream - The stream to close.
     */
    virtual void CloseStream(SinkStream* stream) = 0;

    /**
     * Close all streams.
     */
    virtual void CloseStreams() = 0;

    /**
     * Create a new sink stream, kept within this sink, with a pointer returned for use.
     * Do not free the returned pointer. When done with the stream, call CloseStream on the sink.
     *
     * @param system          - Core system.
     * @param system_channels - Number of channels the audio system expects.
     *                          May differ from the device's channel count.
     * @param name            - Name of this stream.
     * @param type            - Type of this stream, render/in/out.
     *
     * @return A pointer to the created SinkStream
     */
    virtual SinkStream* AcquireSinkStream(Core::System& system, u32 system_channels,
                                          const std::string& name, StreamType type) = 0;

    /**
     * Get the number of channels the playback streams carry now: 2 or 6. A 5.1 game on a
     * stereo output is mixed down to it.
     *
     * @return Number of device channels.
     */
    u32 GetDeviceChannels() const {
        return device_channels.load(std::memory_order_relaxed);
    }

    /**
     * Get the number of channels games are told is active: 6 while surround is allowed,
     * whatever the output, so that a 5.1 game keeps its 5.1 mix when the output changes.
     */
    u32 GetGuestChannels() const {
        return guest_channels.load(std::memory_order_relaxed);
    }

    /**
     * Get the channels the host output played at the last check: 6 when it has 5.1 speakers,
     * otherwise 2 (see SurroundPolicy).
     */
    u32 GetHostChannels() const {
        return host_channels.load(std::memory_order_relaxed);
    }

    /**
     * Match the playback layout to the host output and the user's setting, reopening the
     * playback streams when it changes. Never called from an audio callback.
     *
     * @param allow_surround - False when the user wants stereo.
     * @return True if the layout games are told about changed.
     */
    bool UpdateOutput(bool allow_surround) {
        const bool device_changed = OutputDeviceChanged();
        const u32 host = speaker_layout.HostChannels(QueryHostChannels(), OutputDeviceName());
        if (device_changed || host != GetHostChannels()) {
            // Another output, or the same one set up again: a refused layout gets a new try.
            refused_channels = 0;
        }
        host_channels.store(host, std::memory_order_relaxed);
        u32 wanted = allow_surround ? host : 2U;
        if (wanted == refused_channels) {
            wanted = 2U;
        }
        if (wanted != GetDeviceChannels() || device_changed || RetryClosedPlayback()) {
            if (ReopenPlayback(wanted) < wanted) {
                refused_channels = wanted;
            }
            if (!playback_closed) {
                retry_interval = 1;
                closed_checks = 0;
            }
        }
        const u32 guest = allow_surround ? 6U : 2U;
        return guest_channels.exchange(guest, std::memory_order_relaxed) != guest;
    }

    /// What the playing playback streams receive and play, all 0 if none plays.
    struct PlayingChannels {
        /// The most channels a stream receives from the game
        u32 source;
        /// The fewest channels a stream plays with
        u32 output;
    };

    virtual PlayingChannels GetPlayingChannels() const {
        return {};
    }

    /**
     * Get the device volume. Set from calls to the IAudioDevice service.
     *
     * @return Volume of the device.
     */
    virtual f32 GetDeviceVolume() const = 0;

    /**
     * Set the device volume. Set from calls to the IAudioDevice service.
     *
     * @param volume - New volume of the device.
     */
    virtual void SetDeviceVolume(f32 volume) = 0;

    /**
     * Set the system volume. Comes from the audio system using this stream.
     *
     * @param volume - New volume of the system.
     */
    virtual void SetSystemVolume(f32 volume) = 0;

protected:
    /**
     * Whether the output the streams play on became another device since the last call.
     * A backend that cannot read a device's own layout while it is open closes it here.
     */
    virtual bool OutputDeviceChanged() {
        return false;
    }

    /**
     * Get the number of channels the host output has. May block briefly.
     */
    virtual u32 QueryHostChannels() {
        return 2;
    }

    /**
     * Get the name of the output the streams play on, as the backend lists it, or empty when
     * they follow the system's default output.
     */
    virtual std::string OutputDeviceName() const {
        return {};
    }

    /**
     * Reopen every playback stream with the given number of channels, and store the layout
     * they got in device_channels under the same lock as stream creation.
     *
     * @return The channels the streams play with: fewer when the device refused the layout.
     */
    virtual u32 ReopenPlayback(u32 channels) {
        device_channels.store(channels, std::memory_order_relaxed);
        return channels;
    }

    /// Reopens the playback streams of a backend, all in one layout. Needs its streams lock.
    u32 ReopenStreams(const std::vector<SinkStreamPtr>& streams, u32 channels) {
        u32 opened = channels;
        const auto open_all = [&](u32 layout) {
            playback_closed = false;
            for (const auto& stream : streams) {
                const u32 got = stream->SetDeviceChannels(layout);
                if (got == 0) {
                    // No hardware stream: the output is tried again at a later check.
                    playback_closed = true;
                } else {
                    opened = std::min(opened, got);
                }
            }
        };
        open_all(channels);
        if (opened < channels) {
            // One stream fell back to stereo: the others follow, so that all play the same.
            open_all(opened);
        }
        device_channels.store(opened, std::memory_order_relaxed);
        return opened;
    }

    /// Whether a playback stream left without a hardware stream is due another try: after
    /// one check, then two, up to eight while the output stays unavailable. A new output is
    /// tried at once. Monitor only.
    bool RetryClosedPlayback() {
        if (!playback_closed || ++closed_checks < retry_interval) {
            return false;
        }
        closed_checks = 0;
        retry_interval = std::min(retry_interval * 2, 8U);
        return true;
    }

    /// What the playing playback streams of a backend receive and play. Needs its streams lock.
    static PlayingChannels PlayingChannelsOf(const std::vector<SinkStreamPtr>& streams) {
        PlayingChannels playing{};
        for (const auto& stream : streams) {
            if (stream && stream->GetType() != StreamType::In && !stream->IsPaused()) {
                playing.source = std::max(playing.source, stream->GetSystemChannels());
                const u32 output = stream->GetDeviceChannels();
                playing.output = playing.output == 0 ? output : std::min(playing.output, output);
            }
        }
        return playing;
    }

    /// Channels of the playback streams, read by the renderer and games
    std::atomic<u32> device_channels{2};
    /// Channels the host output played at the last check
    std::atomic<u32> host_channels{2};
    /// Channels games are told is active
    std::atomic<u32> guest_channels{2};
    /// A layout the device refused, not tried again until the output changes. Monitor only.
    u32 refused_channels{};
    /// Reads the speakers of the host output for the surround policy. Monitor only.
    SpeakerLayout speaker_layout;
    /// A playback stream has no hardware stream since the last reopen. Monitor only.
    bool playback_closed{};
    /// Checks since the last try to reopen it, and checks until the next one. Monitor only.
    u32 closed_checks{};
    u32 retry_interval{1};
};

using SinkPtr = std::unique_ptr<Sink>;

} // namespace AudioCore::Sink
