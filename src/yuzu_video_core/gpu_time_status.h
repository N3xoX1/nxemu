// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

namespace VideoCore {
class GpuTimeStatus {
public:
    void Measure(bool measure) {
        measuring.store(measure, std::memory_order_relaxed);
    }
    bool Measuring() const {
        return measuring.load(std::memory_order_relaxed);
    }
    void Add(uint64_t ns) {
        busy_ns.fetch_add(ns, std::memory_order_relaxed);
    }
    uint64_t BusyNs() const {
        return busy_ns.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> measuring{false};
    std::atomic<uint64_t> busy_ns{0};
};
inline GpuTimeStatus gpu_time_status;
} // namespace VideoCore
