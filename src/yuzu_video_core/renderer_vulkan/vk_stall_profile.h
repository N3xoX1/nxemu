// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace Vulkan {
// Opt-in wall-clock attribution for occasional frame stalls. No clock reads when off.
class StallProfile {
public:
    explicit StallProfile(const char* kind_, std::uint64_t tick_ = 0)
        : kind(kind_), tick(tick_), started(Enabled() ? Clock::now() : Clock::time_point{}) {}
    ~StallProfile() {
        if (!Enabled()) return;
        const auto end = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - started).count();
        if (SummaryEnabled()) RecordSummary(kind, ms, end);
        if (ms >= 12.0)
            std::fprintf(stderr, "[NCE-STALL] kind=%s tick=%llu duration_ms=%.3f begin_s=%.6f end_s=%.6f\n",
                kind, (unsigned long long)tick, ms,
                std::chrono::duration<double>(started.time_since_epoch()).count(),
                std::chrono::duration<double>(end.time_since_epoch()).count());
    }
private:
    using Clock = std::chrono::steady_clock;
    static bool Enabled() {
        static const bool enabled = std::getenv("NXEMU_NCE_READBACK_PROFILE") != nullptr;
        return enabled;
    }
    static bool SummaryEnabled() {
        static const bool enabled = std::getenv("NXEMU_NCE_STALL_SUMMARY") != nullptr;
        return enabled;
    }
    struct Bucket {
        const char* name{};
        std::uint64_t calls{};
        double total_ms{}, max_ms{};
        std::array<std::uint64_t, 32> histogram{};
    };
    static void RecordSummary(const char* name, double ms, Clock::time_point end) {
        // Thread-local aggregation adds no shared lock to the submission/wait path.
        struct Summary {
            std::array<Bucket, 6> buckets{};
            Clock::time_point reported{};
            std::uint64_t thread{};
        };
        static std::atomic<std::uint64_t> next_thread{1};
        static thread_local Summary summary{{}, end, next_thread.fetch_add(1, std::memory_order_relaxed)};
        for (auto& bucket : summary.buckets) {
            if (bucket.name && std::strcmp(bucket.name, name) != 0) continue;
            bucket.name = name;
            ++bucket.calls;
            bucket.total_ms += ms;
            if (ms > bucket.max_ms) bucket.max_ms = ms;
            unsigned bin = 0;
            while (bin < 31 && ms * 1000.0 > double(std::uint64_t{1} << bin)) ++bin;
            ++bucket.histogram[bin];
            break;
        }
        const double interval = std::chrono::duration<double>(end - summary.reported).count();
        if (interval < 5.0) return;
        for (auto& bucket : summary.buckets) {
            if (!bucket.calls) continue;
            const auto rank = bucket.calls - bucket.calls / 20;
            std::uint64_t accumulated = 0;
            unsigned percentile = 0;
            for (; percentile < 31; ++percentile) {
                accumulated += bucket.histogram[percentile];
                if (accumulated >= rank) break;
            }
            std::fprintf(stderr,
                "[NCE-STALL-SUMMARY] thread=%llu kind=%s interval=%.6f calls=%llu total_ms=%.3f mean_ms=%.6f p95_upper_ms=%.3f max_ms=%.3f end_s=%.6f\n",
                (unsigned long long)summary.thread, bucket.name, interval,
                (unsigned long long)bucket.calls, bucket.total_ms, bucket.total_ms / static_cast<double>(bucket.calls),
                double(std::uint64_t{1} << percentile) / 1000.0, bucket.max_ms,
                std::chrono::duration<double>(end.time_since_epoch()).count());
            bucket = {};
        }
        summary.reported = end;
    }
    const char* kind;
    std::uint64_t tick;
    Clock::time_point started;
};
} // namespace Vulkan
