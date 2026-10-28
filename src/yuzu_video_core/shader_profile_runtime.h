// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <locale>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace VideoCommon::ShaderProfile::Runtime
{
using Clock = std::chrono::steady_clock;
struct CpuSample
{
    uint64_t ns{}, cycles{};
    bool time_available{}, cycles_available{};
};
inline CpuSample ThreadCpu() noexcept
{
    CpuSample sample;
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
    {
        const auto value = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        sample.ns = (value(kernel) + value(user)) * 100;
        sample.time_available = true;
    }
    ULONG64 cycles{};
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles))
    {
        sample.cycles = cycles;
        sample.cycles_available = true;
    }
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec value{};
    if (!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value))
    {
        sample.ns = uint64_t(value.tv_sec) * 1000000000 + value.tv_nsec;
        sample.time_available = true;
    }
#endif
    return sample;
}
struct Duration
{
    uint64_t start_ns{}, wall_ns{}, cpu_ns{}, calls{}, exceptions{}, cpu_samples{}, cpu_cycles{}, cycle_samples{};
};
enum class Work
{
    Compiler,
    Native,
    Wait,
    DiskCache
};
struct Event
{
    uint64_t hash{}, start_ns{};
    Clock::time_point epoch;
    bool compute{}, quarantined{};
    std::mutex mutex;
    std::array<Duration, 4> work{};
    uint64_t native_successes{}, native_failures{};
};
using Handle = std::shared_ptr<Event>;
inline thread_local Handle current;
class Span
{
public:
    Span(Handle event_, Work work_) :
        event(std::move(event_)), work(work_), exceptions(std::uncaught_exceptions())
    {
        if (event)
        {
            start = Clock::now();
            cpu = ThreadCpu();
        }
    }
    ~Span()
    {
        if (!event) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        const auto end = ThreadCpu();
        std::scoped_lock lock(event->mutex);
        auto & d = event->work[static_cast<size_t>(work)];
        if (!d.calls) d.start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(start - event->epoch).count();
        d.wall_ns += elapsed;
        ++d.calls;
        d.exceptions += std::uncaught_exceptions() > exceptions;
        if (cpu.time_available && end.time_available && end.ns >= cpu.ns)
        {
            d.cpu_ns += end.ns - cpu.ns;
            ++d.cpu_samples;
        }
        if (cpu.cycles_available && end.cycles_available && end.cycles >= cpu.cycles)
        {
            d.cpu_cycles += end.cycles - cpu.cycles;
            ++d.cycle_samples;
        }
    }
    Span(const Span &) = delete;

private:
    Handle event;
    Work work;
    int exceptions;
    Clock::time_point start{};
    CpuSample cpu{};
};
class Context
{
public:
    explicit Context(Handle event) :
        previous(std::move(current))
    {
        current = std::move(event);
    }
    ~Context()
    {
        current = std::move(previous);
    }
    Context(const Context &) = delete;

private:
    Handle previous;
};
inline void NativeResult(const Handle & event, bool success)
{
    if (!event) return;
    std::scoped_lock lock(event->mutex);
    if (success)
        ++event->native_successes;
    else
        ++event->native_failures;
}
// Full keys establish membership; hashes appear only in the diagnostic report.
template <class Key>
class QuarantinedKeys
{
public:
    void Reject(const Key & key)
    {
        ++total;
        if (keys.size() < 4096)
            keys.insert(key);
        else
            truncated = true;
    }
    bool Contains(const Key & key) const
    {
        return keys.contains(key);
    }
    void Requested(const Key & key)
    {
        if (Contains(key))
        {
            ++requests;
            requested.insert(key);
        }
    }
    size_t total{}, requests{};
    bool truncated{};
    std::unordered_set<Key> keys, requested;
};
class Report
{
public:
    explicit Report(std::filesystem::path path_) :
        path(std::move(path_)), epoch(Clock::now())
    {
    }
    bool active{};
    Handle Begin(uint64_t hash, bool compute, bool quarantined)
    {
        if (!active) return {};
        std::scoped_lock lock(mutex);
        if (events.size() >= 65536)
        {
            events_truncated = true;
            return {};
        }
        auto event = std::make_shared<Event>();
        event->hash = hash;
        event->compute = compute;
        event->quarantined = quarantined;
        event->start_ns = Now();
        event->epoch = epoch;
        events.push_back(event);
        return event;
    }
    void Frame()
    {
        const auto now = Now();
        std::scoped_lock lock(mutex);
        if (previous_frame)
        {
            if (frames.size() < 120000)
                frames.push_back({now, now - previous_frame});
            else
                frames_truncated = true;
        }
        previous_frame = now;
    }
    template <class G, class C>
    bool Flush(const G & graphics, const C & compute)
    {
        // Called after all compilers have drained; no file IO occurs during draws.
        std::scoped_lock lock(mutex);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.imbue(std::locale::classic());
        out << "{\"schema\":1,\"clock\":\"steady_ns_since_report_start\",\"events_truncated\":" << events_truncated
            << ",\"frames_truncated\":" << frames_truncated;
        const auto keys = [&](const char * name, const auto & group) { out << ",\"" << name << "\":{\"quarantined\":" << group.total
                                                                           << ",\"tracked\":" << group.keys.size() << ",\"redemanded_unique\":" << group.requested.size()
                                                                           << ",\"slow_requests\":" << group.requests << ",\"truncated\":" << group.truncated << '}'; };
        keys("graphics", graphics);
        keys("compute", compute);
        out << ",\"compilations\":[";
        bool comma = false;
        for (const auto & e : events)
        {
            if (comma) out << ',';
            comma = true;
            std::scoped_lock event_lock(e->mutex);
            out << "{\"key_hash\":\"" << e->hash << "\",\"compute\":" << e->compute << ",\"quarantined\":" << e->quarantined
                << ",\"start_ns\":" << e->start_ns << ",\"native_successes\":" << e->native_successes << ",\"native_failures\":" << e->native_failures;
            const std::array names{"compiler", "native", "wait", "disk_cache"};
            for (size_t i = 0; i < names.size(); ++i)
            {
                const auto & d = e->work[i];
                out << ",\"" << names[i] << "\":{\"wall_ns\":" << d.wall_ns
                    << ",\"start_ns\":" << d.start_ns << ",\"cpu_ns\":" << d.cpu_ns << ",\"calls\":" << d.calls << ",\"exceptions\":" << d.exceptions << ",\"cpu_cycles\":" << d.cpu_cycles << ",\"cycle_samples\":" << d.cycle_samples << ",\"cpu_samples\":" << d.cpu_samples << '}';
            }
            out << '}';
        }
        out << "],\"frames\":[";
        comma = false;
        for (const auto & f : frames)
        {
            if (comma) out << ',';
            comma = true;
            out << '[' << f[0] << ',' << f[1] << ']';
        }
        out << "]}";
        out.flush();
        return bool(out);
    }

private:
    uint64_t Now() const
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - epoch).count();
    }
    std::filesystem::path path;
    Clock::time_point epoch;
    std::mutex mutex;
    std::vector<Handle> events;
    std::vector<std::array<uint64_t, 2>> frames;
    uint64_t previous_frame{};
    bool events_truncated{}, frames_truncated{};
};
} // namespace VideoCommon::ShaderProfile::Runtime
