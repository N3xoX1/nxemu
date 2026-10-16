// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include "yuzu_common/common_types.h"
#include "yuzu_common/hardware_properties.h"
#include "yuzu_common/wall_clock.h"

namespace Core::Timing {

// Events follow the least advanced participating CPU. Scheduler-ready cores
// remain participants across SVCs and fiber switches; cold JIT compilation is
// excluded. Idle time follows the host, and pause freezes every participant.
// This orders timed events without serializing CPU execution or guest memory.
class SharedCpuClock {
    struct alignas(64) Slot {
        std::atomic<u64> base_ns{}, anchor{};
        std::atomic<bool> compiling{};
    };
public:
    void Reset(const Common::WallClock* clock) {
        std::scoped_lock lock{writers};
        host_clock = clock;
        for (auto& slot : slots) {
            slot.base_ns.store(0);
            slot.anchor.store(0);
            slot.compiling.store(false);
        }
        published.store(0);
        active.store(0);
        running.store(0);
        runnable.store(0);
        idle_base.store(0);
        idle_anchor.store(HostNow());
        pace_base.store(0);
        pace_anchor.store(HostNow());
        paused.store(true);
        sequence.store(0);
    }

    u64 Now() const {
        const auto version = sequence.load(std::memory_order_acquire);
        if ((version & 1) || paused.load(std::memory_order_acquire))
            return published.load(std::memory_order_acquire);
        const auto host = HostNow();
        const u32 mask = active.load(std::memory_order_acquire);
        u64 candidate = std::numeric_limits<u64>::max();
        if (!mask) {
            candidate = idle_base.load(std::memory_order_relaxed) +
                host - idle_anchor.load(std::memory_order_relaxed);
        } else {
            for (u32 core = 0; core < slots.size(); ++core) {
                if (!(mask & (1U << core))) continue;
                const auto& slot = slots[core];
                candidate = std::min(candidate, slot.base_ns.load(std::memory_order_relaxed) +
                    (slot.compiling.load(std::memory_order_relaxed) ? 0 :
                     host - slot.anchor.load(std::memory_order_relaxed)));
            }
        }
        candidate = std::min(candidate, pace_base.load(std::memory_order_relaxed) +
            host - pace_anchor.load(std::memory_order_relaxed));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sequence.load(std::memory_order_acquire) != version)
            return published.load(std::memory_order_acquire);
        auto previous = published.load(std::memory_order_relaxed);
        while (previous < candidate && !published.compare_exchange_weak(
            previous, candidate, std::memory_order_release, std::memory_order_relaxed)) {}
        return std::max(previous, candidate);
    }

    // A frozen participant cannot reach this deadline until compilation ends.
    // Take a consistent snapshot; a racing transition simply retries normally.
    bool CompilationBlocks(u64 deadline) const {
        const auto version = sequence.load(std::memory_order_acquire);
        if (version & 1) return false;
        const auto mask = active.load(std::memory_order_acquire);
        bool blocked{};
        for (u32 core = 0; core < slots.size(); ++core) {
            if ((mask & (1U << core)) && slots[core].compiling.load(std::memory_order_relaxed) &&
                slots[core].base_ns.load(std::memory_order_relaxed) < deadline) blocked = true;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        return blocked && sequence.load(std::memory_order_acquire) == version;
    }

    void Begin(u32 core) {
        std::scoped_lock lock{writers};
        const auto now = Now();
        sequence.fetch_add(1, std::memory_order_acq_rel);
        // Pair relaxed payload stores with the reader's acquire fence, so a
        // reader observing a new field also observes the odd sequence.
        std::atomic_thread_fence(std::memory_order_release);
        auto& slot = slots.at(core);
        const auto host = HostNow();
        const bool participates = active.load(std::memory_order_relaxed) & (1U << core);
        const auto own = !participates ? now : slot.base_ns.load(std::memory_order_relaxed) +
            (!paused.load(std::memory_order_relaxed) && !slot.compiling.load(std::memory_order_relaxed) ?
             host - slot.anchor.load(std::memory_order_relaxed) : 0);
        slot.base_ns.store(std::max(now, own), std::memory_order_relaxed);
        slot.anchor.store(host, std::memory_order_relaxed);
        slot.compiling.store(false, std::memory_order_relaxed);
        running.fetch_or(1U << core, std::memory_order_relaxed);
        active.fetch_or(1U << core, std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_release);
    }

    void End(u32 core) {
        std::scoped_lock lock{writers};
        const auto now = Now();
        sequence.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);
        auto& slot = slots.at(core);
        const auto host = HostNow();
        Accumulate(slot, host);
        const auto runs = running.load(std::memory_order_relaxed) & ~(1U << core);
        running.store(runs, std::memory_order_relaxed);
        active.store(runs | runnable.load(std::memory_order_relaxed), std::memory_order_relaxed);
        idle_base.store(now, std::memory_order_relaxed);
        idle_anchor.store(host, std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_release);
    }

    void Pause(bool value) {
        std::scoped_lock lock{writers};
        const auto now = Now();
        const auto host = HostNow();
        sequence.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);
        idle_base.store(now, std::memory_order_relaxed);
        idle_anchor.store(host, std::memory_order_relaxed);
        pace_base.store(now, std::memory_order_relaxed);
        pace_anchor.store(host, std::memory_order_relaxed);
        for (u32 core = 0; core < slots.size(); ++core) {
            auto& slot = slots[core];
            if (active.load(std::memory_order_relaxed) & (1U << core)) Accumulate(slot, host);
            slot.base_ns.store(std::max(now, slot.base_ns.load(std::memory_order_relaxed)), std::memory_order_relaxed);
            slot.anchor.store(host, std::memory_order_relaxed);
        }
        paused.store(value, std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_release);
    }

    // Apply one scheduler snapshot after migrations. A CPU leaves only after
    // its JIT run has also returned; a newly ready CPU starts at common time.
    void SetRunnableMask(u32 mask) {
        if (runnable.load(std::memory_order_relaxed) == mask) return;
        std::scoped_lock lock{writers};
        if (runnable.load(std::memory_order_relaxed) == mask) return;
        const auto now = Now();
        const auto host = HostNow();
        sequence.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);
        const auto previous = active.load(std::memory_order_relaxed);
        for (u32 core = 0; core < slots.size(); ++core) {
            if (!(mask & (1U << core)) || (previous & (1U << core))) continue;
            auto& slot = slots[core];
            slot.base_ns.store(now, std::memory_order_relaxed);
            slot.anchor.store(host, std::memory_order_relaxed);
            slot.compiling.store(false, std::memory_order_relaxed);
        }
        runnable.store(mask, std::memory_order_relaxed);
        active.store(mask | running.load(std::memory_order_relaxed), std::memory_order_relaxed);
        idle_base.store(now, std::memory_order_relaxed);
        idle_anchor.store(host, std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_release);
    }

    void Compile(u32 core, bool value) {
        std::scoped_lock lock{writers};
        if (!(running.load(std::memory_order_relaxed) & (1U << core))) return;
        Now();
        sequence.fetch_add(1, std::memory_order_acq_rel);
        std::atomic_thread_fence(std::memory_order_release);
        auto& slot = slots.at(core);
        const auto host = HostNow();
        Accumulate(slot, host);
        slot.compiling.store(value, std::memory_order_relaxed);
        sequence.fetch_add(1, std::memory_order_release);
    }
private:
    void Accumulate(Slot& slot, u64 host) {
        if (!paused.load(std::memory_order_relaxed) && !slot.compiling.load(std::memory_order_relaxed))
            slot.base_ns.fetch_add(host - slot.anchor.load(std::memory_order_relaxed), std::memory_order_relaxed);
        slot.anchor.store(host, std::memory_order_relaxed);
    }
    u64 HostNow() const { return host_clock->GetTimeNS().count(); }
    std::array<Slot, Hardware::NUM_CPU_CORES> slots{};
    mutable std::mutex writers;
    std::atomic<u64> sequence{}, idle_base{}, idle_anchor{}, pace_base{}, pace_anchor{};
    std::atomic<u32> active{}, running{}, runnable{};
    std::atomic<bool> paused{true};
    mutable std::atomic<u64> published{};
    const Common::WallClock* host_clock{}; // Stable while CPU/timer threads run.
};
} // namespace Core::Timing
