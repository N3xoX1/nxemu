// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <system_error>
#include <thread>
#include "yuzu_common/differential_memory_copy.h"

namespace Common {
// The caller retains its original memory/cache locks until every partition has
// completed. Helpers access only disjoint spans in one contiguous host mapping.
class ReadbackCopyWorkers {
public:
    explicit ReadbackCopyWorkers(unsigned count) : helpers{count > 1 ? std::min(count - 1, 3U) : 0} {
        try {
            for (unsigned i = 0; i < helpers; ++i) {
                workers[i] = std::thread{[this, i] { RunWorker(i); }};
            }
        } catch (...) {
            Stop();
            throw;
        }
    }
    ~ReadbackCopyWorkers() { Stop(); }
    ReadbackCopyWorkers(const ReadbackCopyWorkers&) = delete;
    ReadbackCopyWorkers& operator=(const ReadbackCopyWorkers&) = delete;

    bool CanSplit(const void* destination, const void* source, std::size_t size) const noexcept {
        const auto dst_address = reinterpret_cast<std::uintptr_t>(destination);
        const auto src_address = reinterpret_cast<std::uintptr_t>(source);
        const auto distance = dst_address >= src_address ? dst_address - src_address : src_address - dst_address;
        return helpers && size >= 2 * 1024 * 1024 && distance >= size;
    }
    std::size_t Copy(void* destination, const void* source, std::size_t size) {
        if (!CanSplit(destination, source, size)) {
            return CopyChangedMemory(destination, source, size);
        }
        auto* dst = static_cast<unsigned char*>(destination);
        auto* src = static_cast<const unsigned char*>(source);
        const auto part = (size / (helpers + 1)) & ~std::size_t{65535};
        for (unsigned i = 0; i < helpers; ++i) {
            const auto offset = (i + 1) * part;
            jobs[i] = {dst + offset, src + offset, i == helpers - 1 ? size - offset : part, 0};
        }
        remaining.store(helpers, std::memory_order_relaxed);
        epoch.fetch_add(1, std::memory_order_release);
        epoch.notify_all();
        auto copied = ReadbackDetail::CopyChunks(dst, src, part);
        for (auto pending = remaining.load(std::memory_order_acquire); pending;
             pending = remaining.load(std::memory_order_acquire)) {
            remaining.wait(pending, std::memory_order_acquire);
        }
        for (unsigned i = 0; i < helpers; ++i) copied += jobs[i].copied;
        return copied;
    }

private:
    struct Job {
        unsigned char* destination{};
        const unsigned char* source{};
        std::size_t size{};
        std::size_t copied{};
    };
    void RunWorker(unsigned index) {
        unsigned seen{};
        for (;;) {
            epoch.wait(seen, std::memory_order_acquire);
            seen = epoch.load(std::memory_order_acquire);
            if (stopping.load(std::memory_order_relaxed)) return;
            auto& job = jobs[index];
            job.copied = ReadbackDetail::CopyChunks(job.destination, job.source, job.size);
            remaining.fetch_sub(1, std::memory_order_acq_rel);
            remaining.notify_one();
        }
    }
    void Stop() noexcept {
        stopping.store(true, std::memory_order_relaxed);
        epoch.fetch_add(1, std::memory_order_release);
        epoch.notify_all();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
    }
    const unsigned helpers;
    std::array<Job, 3> jobs{};
    std::atomic<unsigned> epoch{}, remaining{};
    std::atomic<bool> stopping{};
    std::array<std::thread, 3> workers;
};

// Counts include the calling thread. hardware_concurrency reports logical CPUs,
// not physical cores, and may be unknown (zero). Leave capacity for emulation.
inline unsigned ReadbackWorkerCountForHardware(unsigned logical_cpus) {
    return logical_cpus >= 8 ? 4U : logical_cpus >= 4 ? 2U : 0U;
}

inline unsigned ReadbackWorkerCount() {
    static const unsigned count = ReadbackWorkerCountForHardware(std::thread::hardware_concurrency());
    return count;
}

// Own this scope in the retirement thread's normal function body. Joining in a
// Windows DLL's TLS destructor deadlocks: the loader lock prevents helper-thread
// detach while that destructor waits for helpers to exit. Only the nonowning
// pointer is thread-local; all joins happen before the caller enters CRT teardown.
class ReadbackCopyContext {
public:
    ReadbackCopyContext() : previous{current} { current = this; }
    ~ReadbackCopyContext() { current = previous; }
    ReadbackCopyContext(const ReadbackCopyContext&) = delete;
    ReadbackCopyContext& operator=(const ReadbackCopyContext&) = delete;

    static ReadbackCopyContext* Current() noexcept { return current; }
    std::size_t Copy(void* destination, const void* source, std::size_t size) {
        if (!unavailable) {
            try {
                if (!workers) workers = std::make_unique<ReadbackCopyWorkers>(ReadbackWorkerCount());
                return workers->Copy(destination, source, size);
            } catch (const std::system_error&) {
                unavailable = true;
            } catch (const std::bad_alloc&) {
                unavailable = true;
            }
        }
        return CopyChangedMemory(destination, source, size);
    }

private:
    static inline thread_local ReadbackCopyContext* current{};
    ReadbackCopyContext* const previous;
    std::unique_ptr<ReadbackCopyWorkers> workers;
    bool unavailable{};
};

inline std::size_t CopyChangedMemoryForReadback(void* destination, const void* source, std::size_t size) {
    if (size >= 2 * 1024 * 1024 && ReadbackWorkerCount() != 0) {
        if (auto* context = ReadbackCopyContext::Current()) {
            return context->Copy(destination, source, size);
        }
    }
    // Synchronous callers without an explicit retirement scope remain serial.
    return CopyChangedMemory(destination, source, size);
}
} // namespace Common
