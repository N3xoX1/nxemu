// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace Common::Android {

// Owned and driven by one emulation thread. Missing Android APIs leave scheduling unchanged.
// Android NDK API 33+: sum elapsed active execution, excluding idle queue waits.
// Cooperating pipeline threads (such as the async GPU decode thread) can be enrolled
// into the same session via AddThread. Guest CPU threads and the physical GPU are not enrolled.
class PerformanceHintSession {
public:
    PerformanceHintSession();
    ~PerformanceHintSession();
    PerformanceHintSession(const PerformanceHintSession&) = delete;
    PerformanceHintSession& operator=(const PerformanceHintSession&) = delete;

    bool IsAvailable() const;
    void AddThread(int32_t tid);

    void BeginWork();
    void EndWork();
    std::chrono::nanoseconds TakeWorkDuration();
    void ReportFrame(std::chrono::nanoseconds target);

    // Report one frame's active duration directly against its guest frame budget.
    // A zero actual duration with a positive target initializes or updates the session
    // without reporting an outlier sample; a non-positive target closes the session.
    void ReportFrameDuration(std::chrono::nanoseconds actual,
                             std::chrono::nanoseconds target);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Common::Android
