// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace Common::Android {

// Owned and used by one worker thread. Missing APIs leave scheduling unchanged.
// Android NDK API 33+: sum elapsed command execution, excluding queue waits.
// AddThread enrolls peer threads; guest CPU threads and the GPU hardware are not enrolled.
class PerformanceHintSession {
public:
    PerformanceHintSession();
    ~PerformanceHintSession();
    PerformanceHintSession(const PerformanceHintSession&) = delete;
    PerformanceHintSession& operator=(const PerformanceHintSession&) = delete;

    bool IsAvailable() const;
    void AddThread(int32_t tid);
    bool HasThread(int32_t tid) const;

    void BeginWork();
    void EndWork();
    std::chrono::nanoseconds TakeWorkDuration();
    void ReportFrame(std::chrono::nanoseconds target);
    void ReportFrameDuration(std::chrono::nanoseconds actual, std::chrono::nanoseconds target);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Common::Android
