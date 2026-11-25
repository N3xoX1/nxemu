// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <thread>
#include "yuzu_common/android/performance_hint.h"
#include "yuzu_common/dynamic_library.h"

using namespace std::chrono_literals;
struct APerformanceHintManager {};
struct APerformanceHintSession {};

namespace TestAdpf {
APerformanceHintManager manager;
APerformanceHintSession session;
bool manager_available = true, create_fails = false;
int created = 0, closed = 0, reports = 0, updated = 0, error = 0;
int64_t actual = 0, target = 0;
APerformanceHintManager* GetManager() { return manager_available ? &manager : nullptr; }
APerformanceHintSession* Create(APerformanceHintManager*, const int32_t* tids, size_t count,
                                int64_t budget) {
    assert(count == 1 && tids[0] == 73);
    ++created;
    target = budget;
    return create_fails ? nullptr : &session;
}
void Close(APerformanceHintSession* value) { assert(value == &session); ++closed; }
int Update(APerformanceHintSession*, int64_t value) { ++updated; target = value; return error; }
int Report(APerformanceHintSession*, int64_t value) { ++reports; actual = value; return error; }
void* Symbol(const char* name) {
    if (!std::strcmp(name, "APerformanceHint_getManager")) return reinterpret_cast<void*>(GetManager);
    if (!std::strcmp(name, "APerformanceHint_createSession")) return reinterpret_cast<void*>(Create);
    if (!std::strcmp(name, "APerformanceHint_closeSession")) return reinterpret_cast<void*>(Close);
    if (!std::strcmp(name, "APerformanceHint_updateTargetWorkDuration")) return reinterpret_cast<void*>(Update);
    if (!std::strcmp(name, "APerformanceHint_reportActualWorkDuration")) return reinterpret_cast<void*>(Report);
    assert(false);
    return nullptr;
}
std::chrono::nanoseconds Work(Common::Android::PerformanceHintSession& hint) {
    const auto start = std::chrono::steady_clock::now();
    hint.BeginWork();
    const auto until = std::chrono::steady_clock::now() + 1ms;
    while (std::chrono::steady_clock::now() < until) {}
    hint.EndWork();
    return std::chrono::steady_clock::now() - start;
}
}

int main() {
    using Common::Android::PerformanceHintSession;
    using namespace TestAdpf;
    constexpr std::array missing_symbols{"", "", "APerformanceHint_getManager",
        "APerformanceHint_createSession", "APerformanceHint_closeSession",
        "APerformanceHint_updateTargetWorkDuration", "APerformanceHint_reportActualWorkDuration"};
    for (size_t missing = 0; missing != missing_symbols.size(); ++missing) {
        library_available = missing != 0;
        manager_available = missing != 1;
        missing_symbol = missing_symbols[missing];
        PerformanceHintSession unavailable;
        unavailable.BeginWork(); unavailable.EndWork(); unavailable.ReportFrame(16ms);
        assert(created == 0);
    }
    library_available = manager_available = true;
    missing_symbol = "";
    {
        PerformanceHintSession hint;
        Work(hint); hint.ReportFrame(16ms);
        assert(created == 1 && reports == 0 && target == 16'000'000);
        hint.EndWork(); // An unmatched end must not count time since the epoch.
        const auto elapsed = Work(hint); hint.ReportFrame(16ms);
        // A loaded CI runner may preempt Work for more than 100 ms. Compare
        // against its measured interval rather than a fixed scheduling limit.
        assert(reports == 1 && actual > 0 && actual <= elapsed.count());
        Work(hint); hint.ReportFrame(33ms);
        assert(updated == 1 && reports == 1 && target == 33'000'000);
        Work(hint); hint.ReportFrame(33ms);
        assert(reports == 2);
        hint.ReportFrame(0ns);
        assert(closed == 1);
        Work(hint); hint.ReportFrame(8ms);
        assert(created == 2 && reports == 2);
        std::this_thread::sleep_for(270ms); // Resume discards the previous partial period.
        Work(hint); hint.ReportFrame(8ms);
        assert(created == 3 && closed == 2 && reports == 2);
        error = 32; // EPIPE must close the session and stop retrying.
        Work(hint); hint.ReportFrame(8ms);
        assert(closed == 3 && reports == 3);
        Work(hint); hint.ReportFrame(8ms);
        assert(created == 3 && reports == 3);
    }
    assert(closed == 3);
    error = 0;
    {
        PerformanceHintSession hint;
        hint.ReportFrame(16ms);
        error = 32;
        hint.ReportFrame(33ms);
        assert(updated == 2 && closed == 4);
        hint.ReportFrame(16ms);
        assert(created == 4);
    }
    create_fails = true;
    {
        PerformanceHintSession hint;
        hint.ReportFrame(16ms); hint.ReportFrame(16ms);
        assert(created == 5);
    }
    assert(closed == 4);
    create_fails = false;
    error = 0;
    {
        PerformanceHintSession hint;
        hint.ReportFrame(16ms);
    }
    assert(created == 6 && closed == 5); // Worker exit closes its session once.
    {
        PerformanceHintSession hint;
        hint.ReportFrame(16ms);
        const auto elapsed = Work(hint);
        // Do not BeginWork after this wait: an oversleep past the 250 ms
        // resume threshold is valid behavior and would recreate the session.
        std::this_thread::sleep_for(150ms);
        hint.ReportFrame(16ms);
        assert(created == 7 && reports == 4 && actual > 0 && actual <= elapsed.count());
    }
    assert(closed == 6);

}
