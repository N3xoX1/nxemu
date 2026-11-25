// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>

namespace Common::Android {

// Owned and used by one worker thread. Missing APIs leave scheduling unchanged.
// Android NDK API 33+: sum elapsed command execution, excluding queue waits.
// Guest CPU threads and the GPU hardware are not enrolled in this session.
class PerformanceHintSession {
public:
    PerformanceHintSession();
    ~PerformanceHintSession();
    PerformanceHintSession(const PerformanceHintSession&) = delete;
    PerformanceHintSession& operator=(const PerformanceHintSession&) = delete;

    void BeginWork();
    void EndWork();
    void ReportFrame(std::chrono::nanoseconds target);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Common::Android
