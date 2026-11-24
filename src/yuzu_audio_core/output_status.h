// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_common/common_types.h"

namespace AudioCore {

/**
 * What the output plays, refreshed by the output monitor for the interface. All zero while no
 * game runs. The fields come from one monitor pass, so they always agree with each other.
 */
struct OutputStatus {
    /// Channels the playback streams carry: 2 or 6
    u32 output_channels;
    /// The most channels the host output plays: 2 or 6
    u32 host_channels;
    /// The most channels a playing stream gets from the game, 0 if none plays
    u32 source_channels;
    /// Whether the output was set up with surround allowed (stereo not forced)
    bool surround_allowed;
};

/// Readable from any thread without touching the audio core.
OutputStatus PublishedOutputStatus();

} // namespace AudioCore
