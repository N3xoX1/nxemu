// SPDX-License-Identifier: GPL-2.0-or-later

#include "yuzu_common/android/performance_hint.h"

#include <cstdint>
#include <utility>
#if defined(__ANDROID__) && defined(NXEMU_ENABLE_ANDROID_ADPF)
#include <unistd.h>

#include "yuzu_common/dynamic_library.h"
#include "yuzu_common/logging/log.h"

// Resolve API 33 functions at runtime to retain Android API 28 compatibility.
struct APerformanceHintManager;
struct APerformanceHintSession;

namespace Common::Android {

struct PerformanceHintSession::Impl {
    using Clock = std::chrono::steady_clock;
    using GetManager = APerformanceHintManager* (*)();
    using CreateSession = APerformanceHintSession* (*)(APerformanceHintManager*, const int32_t*,
                                                     size_t, int64_t);
    using CloseSession = void (*)(APerformanceHintSession*);
    using UpdateTarget = int (*)(APerformanceHintSession*, int64_t);
    using ReportActual = int (*)(APerformanceHintSession*, int64_t);

    Impl() {
        GetManager get_manager{};
        if (library.IsOpen() &&
            library.GetSymbol("APerformanceHint_getManager", &get_manager) &&
            library.GetSymbol("APerformanceHint_createSession", &create_session) &&
            library.GetSymbol("APerformanceHint_closeSession", &close_session) &&
            library.GetSymbol("APerformanceHint_updateTargetWorkDuration", &update_target) &&
            library.GetSymbol("APerformanceHint_reportActualWorkDuration", &report_actual)) {
            manager = get_manager();
        }
    }

    ~Impl() {
        Close();
    }

    void Close() {
        if (session) {
            close_session(std::exchange(session, nullptr));
        }
    }

    void Disable(int result) {
        LOG_DEBUG(Common, "Android performance hint session disabled: error {}", result);
        Close();
        failed = true;
    }

    DynamicLibrary library{"libandroid.so"};
    APerformanceHintManager* manager{};
    APerformanceHintSession* session{};
    CreateSession create_session{};
    CloseSession close_session{};
    UpdateTarget update_target{};
    ReportActual report_actual{};
    Clock::time_point work_start{};
    Clock::time_point last_work_end{};
    std::chrono::nanoseconds work_duration{};
    std::chrono::nanoseconds target{};
    bool failed{};
};

PerformanceHintSession::PerformanceHintSession() : impl{std::make_unique<Impl>()} {}
PerformanceHintSession::~PerformanceHintSession() = default;

void PerformanceHintSession::BeginWork() {
    if (impl->manager && !impl->failed) {
        const auto now = Impl::Clock::now();
        if (impl->last_work_end != Impl::Clock::time_point{} &&
            now - impl->last_work_end > std::chrono::milliseconds{250}) {
            // A resumed frame must not inherit partial pre-pause/loading work.
            impl->Close();
            impl->work_duration = {};
        }
        impl->work_start = now;
    }
}

void PerformanceHintSession::EndWork() {
    if (impl->manager && !impl->failed && impl->work_start != Impl::Clock::time_point{}) {
        const auto now = Impl::Clock::now();
        impl->work_duration += std::chrono::duration_cast<std::chrono::nanoseconds>(
            now - impl->work_start);
        impl->last_work_end = now;
        impl->work_start = {};
    }
}

void PerformanceHintSession::ReportFrame(std::chrono::nanoseconds target) {
    const auto actual = std::exchange(impl->work_duration, std::chrono::nanoseconds{});
    if (!impl->manager || impl->failed) {
        return;
    }
    if (target <= std::chrono::nanoseconds{} || target > std::chrono::seconds{1}) {
        // Unlocked emulation has no frame deadline. Recreate if a limit returns.
        impl->Close();
        return;
    }
    if (!impl->session) {
        const int32_t tid = gettid();
        impl->session = impl->create_session(impl->manager, &tid, 1, target.count());
        if (!impl->session) {
            LOG_DEBUG(Common, "Android performance hint session unavailable on this device");
            impl->failed = true;
        } else {
            impl->target = target;
        }
        // The first sample can include loading work before the first frame.
        return;
    }
    if (target != impl->target) {
        if (const int result = impl->update_target(impl->session, target.count()); result != 0) {
            impl->Disable(result);
            return;
        }
        impl->target = target;
        // This sample may straddle two different guest cadences.
        return;
    }
    if (actual > std::chrono::nanoseconds{}) {
        if (const int result = impl->report_actual(impl->session, actual.count()); result != 0) {
            impl->Disable(result);
        }
    }
}

} // namespace Common::Android

#else

namespace Common::Android {
struct PerformanceHintSession::Impl {};
PerformanceHintSession::PerformanceHintSession() = default;
PerformanceHintSession::~PerformanceHintSession() = default;
void PerformanceHintSession::BeginWork() {}
void PerformanceHintSession::EndWork() {}
void PerformanceHintSession::ReportFrame(std::chrono::nanoseconds) {}
} // namespace Common::Android

#endif
