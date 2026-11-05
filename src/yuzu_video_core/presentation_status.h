// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <mutex>
#include "nxemu-module-spec/video.h"

namespace VideoCore {
// A single coherent snapshot crosses the renderer/UI threads. The lock never
// covers GPU work, guest memory, settings callbacks or I/O.
class PresentationStatus {
public:
    void Publish(VideoPresentationState state) {
        std::scoped_lock lock{mutex};
        latest = state;
    }
    VideoPresentationState Read() const {
        std::scoped_lock lock{mutex};
        return latest;
    }
    void Reset() { Publish({}); }
private:
    mutable std::mutex mutex;
    VideoPresentationState latest{};
};
inline PresentationStatus presentation_status;
} // namespace VideoCore
