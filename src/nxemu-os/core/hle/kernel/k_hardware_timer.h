// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <limits>

#include "core/hle/kernel/k_hardware_timer_base.h"
#include "yuzu_common/wall_clock.h"

namespace Core::Timing {
struct EventType;
} // namespace Core::Timing

namespace Kernel {

class KHardwareTimer : /* public KInterruptTask, */ public KHardwareTimerBase {
public:
    // SVC timeouts include two guest timer ticks. Our deadlines remain in
    // nanoseconds, so round this margin up rather than adding two nanoseconds.
    static constexpr s64 DefaultTimeIncrementNanoseconds =
        (2 * 1'000'000'000LL + Common::WallClock::CNTFRQ - 1) / Common::WallClock::CNTFRQ;
    static_assert(DefaultTimeIncrementNanoseconds == 105);

    explicit KHardwareTimer(KernelCore& kernel) : KHardwareTimerBase{kernel} {}

    // Public API.
    void Initialize();
    void Finalize();

    s64 GetTick() const;

    s64 GetTimeoutDeadlineNs(s64 timeout_ns) const {
        // Preserve immediate polling and infinite waits without reading the clock.
        if (timeout_ns <= 0) {
            return timeout_ns;
        }

        constexpr s64 max_deadline = (std::numeric_limits<s64>::max)();
        if (timeout_ns > max_deadline - DefaultTimeIncrementNanoseconds) {
            return max_deadline;
        }

        const s64 offset_ns = timeout_ns + DefaultTimeIncrementNanoseconds;
        const s64 now_ns = GetTick();
        // The timer is nonnegative. Check the remaining range before adding;
        // a check after signed overflow would already be undefined behavior.
        if (static_cast<u64>(now_ns) > static_cast<u64>(max_deadline - offset_ns)) {
            return max_deadline;
        }
        return now_ns + offset_ns;
    }

    void RegisterAbsoluteTask(KTimerTask* task, s64 task_time) {
        KScopedDisableDispatch dd{m_kernel};
        KScopedSpinLock lk{this->GetLock()};

        if (this->RegisterAbsoluteTaskImpl(task, task_time)) {
            if (task_time <= m_wakeup_time) {
                this->EnableInterrupt(task_time);
            }
        }
    }

private:
    void EnableInterrupt(s64 wakeup_time);
    void DisableInterrupt();
    bool GetInterruptEnabled();
    void DoTask();

private:
    // Absolute time in nanoseconds
    s64 m_wakeup_time{std::numeric_limits<s64>::max()};
    std::shared_ptr<Core::Timing::EventType> m_event_type{};
};

} // namespace Kernel
