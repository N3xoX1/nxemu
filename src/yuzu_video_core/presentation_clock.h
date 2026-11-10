// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>

namespace VideoCore {

/// Time of the last image shown on the host window. Presentation stops while the window is
/// moved or resized on most systems, because the window loop is blocked, while the guest keeps
/// running and the host GPU falls behind it.
class PresentationClock {
public:
    using Clock = std::chrono::steady_clock;

    static void MarkPresented() noexcept {
        last_present.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    }

    /// True when nothing was presented for longer than the limit
    [[nodiscard]] static bool Stalled(Clock::time_point now, Clock::duration limit) noexcept {
        const auto last = last_present.load(std::memory_order_relaxed);
        return last != 0 && now - Clock::time_point(Clock::duration(last)) > limit;
    }

private:
    static inline std::atomic<Clock::rep> last_present{0};
};

} // namespace VideoCore
