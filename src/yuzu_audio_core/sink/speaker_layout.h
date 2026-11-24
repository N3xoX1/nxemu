// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string_view>

#include "yuzu_common/common_types.h"

namespace AudioCore::Sink {

/// Speaker positions of an output, one bit each.
namespace Speaker {
constexpr u32 FrontLeft = 1U << 0;
constexpr u32 FrontRight = 1U << 1;
constexpr u32 FrontCenter = 1U << 2;
constexpr u32 LowFrequency = 1U << 3;
constexpr u32 BackLeft = 1U << 4;
constexpr u32 BackRight = 1U << 5;
constexpr u32 SideLeft = 1U << 6;
constexpr u32 SideRight = 1U << 7;
} // namespace Speaker

/**
 * The surround policy of every backend and system: the channels the host plays, 6 or 2.
 *
 * A channel count alone is not surround: the outputs of an audio interface are discrete,
 * usually with stereo monitors on the first two, so 5.1 would put the center and back channels
 * on outputs with nothing connected. An output plays 5.1 only when it has six channels or more
 * and its speakers include the front pair, the center and a surround pair, back or side. The
 * LFE is optional, so 5.0 speakers stay surround. An output whose speakers the system does not
 * report plays stereo, the downmix keeping every channel audible.
 *
 * @param channels - The channels the output has.
 * @param speakers - Its speaker positions, std::nullopt when unknown.
 */
constexpr u32 SurroundPolicy(u32 channels, std::optional<u32> speakers) {
    using namespace Speaker;
    if (channels < 6 || !speakers) {
        return 2;
    }
    const auto has = [&](u32 positions) { return (*speakers & positions) == positions; };
    const bool surround_pair = has(BackLeft | BackRight) || has(SideLeft | SideRight);
    return has(FrontLeft | FrontRight | FrontCenter) && surround_pair ? 6 : 2;
}

/**
 * Applies the surround policy to the system's outputs, reading their speaker layout: on macOS
 * the positions Core Audio maps a 5.1 stream onto through the device's preferred layout (Audio
 * MIDI Setup, Configure Speakers; HDMI: the sink's own), on Windows the channel mask of the
 * endpoint's format (Sound settings, Configure), on Linux the channel map of the PulseAudio or
 * PipeWire sink. Elsewhere the speakers are unknown, so the output plays stereo.
 *
 * Reads the system on each call, so a speaker change applies at the next check. Keeps a
 * connection to the sound server where the system needs one. Use from one thread at a time,
 * never from an audio callback.
 */
class SpeakerLayout {
public:
    SpeakerLayout();
    ~SpeakerLayout();

    SpeakerLayout(const SpeakerLayout&) = delete;
    SpeakerLayout& operator=(const SpeakerLayout&) = delete;

    /**
     * Get the channels the host plays on an output, logging when the policy limits it.
     *
     * @param channels    - The channels the output has.
     * @param device_name - The output's name as the backend lists it, empty for the default.
     * @return 6 or 2.
     */
    u32 HostChannels(u32 channels, std::string_view device_name);

    /**
     * Get the speaker positions of an output.
     *
     * @param device_name - The output's name as the backend lists it. An empty or unknown
     *                      name reads the default output.
     * @return The positions, std::nullopt when the system does not report them.
     */
    std::optional<u32> QuerySpeakers(std::string_view device_name);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    /// The last result of HostChannels, so that only changes are logged
    std::optional<u32> logged_speakers;
    u32 logged_channels{};
};

static_assert(SurroundPolicy(6, Speaker::FrontLeft | Speaker::FrontRight | Speaker::FrontCenter |
                                    Speaker::LowFrequency | Speaker::SideLeft |
                                    Speaker::SideRight) == 6);
static_assert(SurroundPolicy(6, Speaker::FrontLeft | Speaker::FrontRight | Speaker::FrontCenter |
                                    Speaker::BackLeft | Speaker::BackRight) == 6);
static_assert(SurroundPolicy(22, Speaker::FrontLeft | Speaker::FrontRight) == 2);
static_assert(SurroundPolicy(8, std::nullopt) == 2);
static_assert(SurroundPolicy(6, Speaker::FrontLeft | Speaker::FrontRight | Speaker::BackLeft |
                                    Speaker::BackRight) == 2);
static_assert(SurroundPolicy(2, 0xFF) == 2);

} // namespace AudioCore::Sink
