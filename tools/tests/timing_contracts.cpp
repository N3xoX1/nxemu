// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include "yuzu_common/wall_clock.h"
// Only clock inputs are substituted. The production class and timing methods
// are extracted from the selected repository on each run.
struct FakeSystemClock {
    static inline std::chrono::nanoseconds value{};
    static auto now() { return std::chrono::time_point<FakeSystemClock, std::chrono::nanoseconds>{value}; }
};
struct FakeSteadyClock {
    static inline std::chrono::nanoseconds value{};
    static auto now() { return std::chrono::time_point<FakeSteadyClock, std::chrono::nanoseconds>{value}; }
};
namespace Common {
#include "standard_wall_clock.inc"
}
constexpr s64 MAX_SLICE_LENGTH = 10000;
struct CoreTiming {
    u64 cpu_ticks = 0;
    s64 downcount = MAX_SLICE_LENGTH;
    void AddTicks(u64 ticks_to_add);
    void ResetTicks();
};
#include "core_timing_ticks.inc"

int main() {
    using namespace std::chrono;
    int failed = 0;
    int total = 0;
    auto check = [&](bool result, const char* name) {
        std::printf("%s: %s\n", result ? "PASS" : "FAIL", name);
        failed += !result;
        ++total;
    };
    CoreTiming timing;
    timing.AddTicks(100);
    timing.AddTicks(200);
    std::printf("After 100+200 ticks: remaining=%lld expected=9700\n", timing.downcount);
    check(timing.cpu_ticks == 300 && timing.downcount == 9700,
          "multiple CPU callbacks consume only their increments");
    timing.ResetTicks();
    timing.AddTicks(50);
    check(timing.cpu_ticks == 350 && timing.downcount == 9950,
          "new CPU slice is independent of lifetime accumulated ticks");
    timing.AddTicks(20000);
    timing.ResetTicks();
    timing.AddTicks(1);
    std::printf("New slice after accumulated ticks: remaining=%lld expected=9999\n", timing.downcount);
    check(timing.cpu_ticks == 20351 && timing.downcount == 9999,
          "old CPU work cannot immediately exhaust a new slice");

    FakeSystemClock::value = seconds{1000};
    FakeSteadyClock::value = seconds{100};
    Common::StandardWallClock clock;
    auto ns = clock.GetTimeNS();
    auto us = clock.GetTimeUS();
    auto ms = clock.GetTimeMS();
    FakeSystemClock::value -= seconds{2};
    FakeSteadyClock::value += milliseconds{10};
    std::printf("After wall time -2s and steady time +10ms: scheduler delta=%lld ns\n",
                (clock.GetTimeNS() - ns).count());
    check(clock.GetTimeNS() - ns == milliseconds{10}, "NS timer ignores backwards wall correction");
    check(clock.GetTimeUS() - us == milliseconds{10}, "US timer ignores backwards wall correction");
    check(clock.GetTimeMS() - ms == milliseconds{10}, "MS timer ignores backwards wall correction");
    ns = clock.GetTimeNS();
    us = clock.GetTimeUS();
    ms = clock.GetTimeMS();
    FakeSystemClock::value += seconds{3600};
    FakeSteadyClock::value += milliseconds{10};
    check(clock.GetTimeNS() - ns == milliseconds{10}, "NS timer ignores forwards wall correction");
    check(clock.GetTimeUS() - us == milliseconds{10}, "US timer ignores forwards wall correction");
    check(clock.GetTimeMS() - ms == milliseconds{10}, "MS timer ignores forwards wall correction");
    check(clock.GetTimeNS().count() == clock.GetUptime(), "scheduler and guest counter source share the monotonic epoch");
    std::printf("%d/%d passed\n", total - failed, total);
    return failed ? 1 : 0;
}
