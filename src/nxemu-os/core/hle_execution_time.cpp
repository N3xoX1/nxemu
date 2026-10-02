// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/hle_execution_time.h"
#include <limits>
#include <thread>
#include <system_error>
#if defined(_WIN32) && (defined(_M_X64) || defined(ARCHITECTURE_x86_64))
#include <Windows.h>
#undef CreateEvent
#include "yuzu_common/x64/cpu_detect.h"
#include "yuzu_common/x64/rdtsc.h"
#elif !defined(_WIN32)
#include <time.h>
#endif

namespace Core::Timing {
#if defined(_WIN32) && (defined(_M_X64) || defined(ARCHITECTURE_x86_64))
namespace {
// QueryThreadCycleTime does not promise a portable conversion to time. Accept
// its TSC conversion only after checking the counter domain on every logical
// processor on which a migrated emulation thread could execute.
bool HasCompatibleThreadCounter(u64 frequency) {
    GROUP_AFFINITY previous{};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &previous)) return false;
    bool compatible = true;
    for (WORD group = 0; group < GetActiveProcessorGroupCount() && compatible; ++group) {
        for (DWORD index = 0; index < GetActiveProcessorCount(group) && compatible; ++index) {
            GROUP_AFFINITY affinity{};
            affinity.Group = group;
            affinity.Mask = KAFFINITY{1} << index;
            if (!SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr)) {
                compatible = false;
                break;
            }
            bool matched = false;
            // TSC includes interrupts/descheduling; the thread counter does not.
            // A 0.5 ms window could reject a compatible host during startup.
            // Use a longer sample and bounded retries without relaxing the
            // counter-domain tolerance. This runs only during initialization.
            for (int attempt = 0; attempt < 8 && !matched; ++attempt) {
                ULONG64 first{}, last{};
                if (!QueryThreadCycleTime(GetCurrentThread(), &first)) break;
                const auto start = Common::X64::FencedRDTSC();
                // Short busy sample. Descheduling can only make the comparison
                // too small; retry rather than accepting a different rate.
                u64 end;
                do { end = Common::X64::FencedRDTSC(); } while (end - start < frequency / 500);
                if (!QueryThreadCycleTime(GetCurrentThread(), &last) || last < first) break;
                const auto elapsed = end - start;
                const auto cycles = last - first;
                matched = elapsed && cycles >= elapsed - elapsed / 20 &&
                    cycles <= elapsed + elapsed / 20;
            }
            compatible = matched;
        }
    }
    const bool restored = SetThreadGroupAffinity(GetCurrentThread(), &previous, nullptr) != 0;
    return compatible && restored;
}
} // namespace
#endif

std::optional<u64> ReadNativeThreadTimeNs() {
#if defined(_WIN32) && (defined(_M_X64) || defined(ARCHITECTURE_x86_64))
    // Convert only on hosts whose thread counter was checked against the
    // invariant TSC on every logical processor. Turbo CPU and QPC frequencies
    // are not interchangeable with this validated counter domain.
    static const u64 frequency = [] {
        const auto& caps = Common::GetCPUCaps();
        if (!caps.invariant_tsc) return u64{};
        // CPUID may expose a ratio without its crystal frequency, or omit leaf
        // 0x15 entirely. Calibrate the invariant TSC once before services start.
        const auto rate = caps.tsc_frequency ? caps.tsc_frequency : Common::X64::EstimateRDTSCFrequency();
        if (!rate) return u64{};
        // Probe affinity on a short-lived worker: even a failed restoration
        // must not alter the application thread on an unsupported host.
        bool compatible{};
        try {
            std::thread probe{[&] { compatible = HasCompatibleThreadCounter(rate); }};
            probe.join();
        } catch (const std::system_error&) {
            return u64{};
        }
        return compatible ? rate : u64{};
    }();
    if (!frequency || frequency > (std::numeric_limits<u64>::max)() / 1'000'000'000)
        return std::nullopt;
    ULONG64 ticks{};
    if (!QueryThreadCycleTime(GetCurrentThread(), &ticks)) return std::nullopt;
    const auto fraction = ticks % frequency * 1'000'000'000 / frequency;
    const auto seconds = ticks / frequency;
    if (seconds > ((std::numeric_limits<u64>::max)() - fraction) / 1'000'000'000)
        return std::nullopt;
    return seconds * 1'000'000'000 + fraction;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return std::nullopt;
    return static_cast<u64>(time.tv_sec) * 1'000'000'000 + static_cast<u64>(time.tv_nsec);
#else
    return std::nullopt;
#endif
}
} // namespace Core::Timing
