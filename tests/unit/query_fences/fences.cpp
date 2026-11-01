// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
#include <semaphore>
#include <string_view>
#include "yuzu_video_core/fence_manager.h"

namespace Settings {
std::atomic<bool> exact_readback{};
bool IsGpuReadbackExact() { return exact_readback.load(std::memory_order_relaxed); }
}
namespace Common {
void SetCurrentThreadName(const char*) {}
void SetCurrentThreadPriority(ThreadPriority) {}
}

namespace {
// Fake the GPU's completion signal and cache contents; exercise the production
// fence queue, flush retirement, callbacks and asynchronous worker unchanged.
struct Cache {
    std::mutex mutex;
    bool pending{};
    bool pending_on_presync{};
    mutable std::mutex slot_guard;
    std::deque<bool> slots;
    std::atomic<unsigned> retired{};
    std::atomic<unsigned> result{};
    std::function<void()> on_commit;
    std::function<void()> before_pop;
    bool protects_pending{};
    bool HasUncommittedFlushes() const { return pending; }
    bool PendingDownloadsProtected() const { return protects_pending; }
    void AccumulateFlushes() {}
    void NotifyWFI() {
        pending |= std::exchange(pending_on_presync, false);
    }
    void PrepareAsyncFlushes() {
        NotifyWFI();
    }
    bool ShouldWaitAsyncFlushes() const {
        std::scoped_lock lock(slot_guard);
        return !slots.empty() && slots.front();
    }
    void CommitAsyncFlushes() {
        NotifyWFI();
        if (on_commit) on_commit();
        std::scoped_lock lock(slot_guard);
        slots.push_back(std::exchange(pending, false));
    }
    void PopAsyncFlushes() {
        if (before_pop) before_pop();
        std::scoped_lock lock(slot_guard);
        assert(!slots.empty());
        if (slots.front()) result.store(42, std::memory_order_release);
        slots.pop_front();
        ++retired;
    }
};
struct Fence : VideoCommon::FenceBase {
    explicit Fence(bool stub) : FenceBase{stub} {}
};
template <bool async> struct Traits {
    using FenceType = std::shared_ptr<Fence>;
    using BufferCacheType = Cache;
    using TextureCacheType = Cache;
    using QueryCacheType = Cache;
    static constexpr bool HAS_ASYNC_CHECK = async;
};
template <bool async>
class Manager final : public VideoCommon::FenceManager<Traits<async>> {
    using Base = VideoCommon::FenceManager<Traits<async>>;
public:
    Manager(VideoCore::RasterizerInterface& r, Tegra::GPU& gpu, Cache& t, Cache& b, Cache& q)
        : Base{r, gpu, t, b, q} {}
    ~Manager() {
        // Drain while the derived virtual fence implementation still exists.
        SignalGpu();
        this->WaitForFence();
    }
    void SignalGpu() { ready.store(true, std::memory_order_release); ready.notify_all(); }
    std::atomic<bool> ready{};
    std::atomic<unsigned> host_fences{};
    std::atomic<unsigned> waits{};
    std::function<void()> on_wait;
protected:
    std::shared_ptr<Fence> CreateFence(bool stub) override {
        if (!stub) ++host_fences;
        return std::make_shared<Fence>(stub);
    }
    void QueueFence(std::shared_ptr<Fence>&) override {}
    bool IsFenceSignaled(std::shared_ptr<Fence>& fence) const override {
        return fence->IsStubbed() || ready.load(std::memory_order_acquire);
    }
    void WaitFence(std::shared_ptr<Fence>& fence) override {
        if (fence->IsStubbed()) return;
        ++waits;
        if (on_wait) on_wait();
        ready.wait(false, std::memory_order_acquire);
    }
};

// Hold retirement at a known point, without depending on worker scheduling.
struct RetirementGate {
    std::binary_semaphore entered{0};
    std::binary_semaphore released{0};
    std::atomic<bool> used{};
    void StopOnce() {
        if (!used.exchange(true)) {
            entered.release();
            released.acquire();
        }
    }
    void AwaitEntry() {
        assert(entered.try_acquire_for(std::chrono::seconds{5}));
    }
};

void CheckNormalBacklog() {
    Settings::exact_readback.store(false);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    RetirementGate gate;
    Manager<true> manager{rasterizer, gpu, textures, buffers, queries};
    queries.before_pop = [&] { gate.StopOnce(); };
    // Vulkan adds this bookkeeping even when no query readback is pending.
    queries.on_commit = [&] { manager.SyncOperation([] {}); };
    unsigned completed = 0;
    manager.SignalFence([&] { ++completed; });
    gate.AwaitEntry(); // An ordinary stub is now in flight; its callback ran inline.
    assert(completed == 1);
    for (unsigned i = 0; i < 4096; ++i) {
        manager.SignalFence([&] { ++completed; });
        assert(completed == i + 2); // Neither in-flight nor queued stubs justify delay.
    }
    assert(manager.host_fences == 0 && manager.waits == 0 && rasterizer.flushes == 0);
    gate.released.release();
    manager.WaitForFence();
    assert(textures.retired == buffers.retired && buffers.retired == queries.retired);
}

void CheckQueuedReadback() {
    Settings::exact_readback.store(false);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    RetirementGate gate, gpu_gate;
    Manager<true> manager{rasterizer, gpu, textures, buffers, queries};
    queries.before_pop = [&] { gate.StopOnce(); };
    manager.on_wait = [&] { gpu_gate.StopOnce(); };
    std::atomic<unsigned> completed{};
    manager.SignalFence([&] { ++completed; });
    gate.AwaitEntry();
    // An inline prefix must be included when locating the last delayed fence.
    for (unsigned i = 0; i < 8; ++i) manager.SignalFence([&] { ++completed; });
    assert(completed == 9);
    queries.pending = true;
    manager.SignalFence([&] {
        assert(queries.result.load(std::memory_order_acquire) == 42);
        assert(completed.fetch_add(1) == 9);
    });
    manager.SignalFence([&] { assert(completed.fetch_add(1) == 10); });
    assert(completed == 9);
    gate.released.release();
    gpu_gate.AwaitEntry(); // The inline prefix has retired; the root is now in flight.
    manager.SignalFence([&] { assert(completed.fetch_add(1) == 11); });
    assert(completed == 9);
    manager.SignalGpu();
    gpu_gate.released.release();
    manager.WaitForFence();
    assert(completed == 12 && manager.host_fences == 1 && rasterizer.flushes == 1);
}

void CheckHighBacklog() {
    Settings::exact_readback.store(true);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    RetirementGate gate;
    Manager<true> manager{rasterizer, gpu, textures, buffers, queries};
    queries.before_pop = [&] { gate.StopOnce(); };
    std::atomic<unsigned> completed{};
    manager.SignalFence([&] { assert(completed.fetch_add(1) == 0); });
    gate.AwaitEntry();
    manager.SignalFence([&] { assert(completed.fetch_add(1) == 1); });
    assert(completed == 0);
    gate.released.release();
    manager.WaitForFence();
    assert(completed == 2 && manager.host_fences == 0 && manager.waits == 0);
}

template <bool async>
void CheckNormalResumes() {
    Settings::exact_readback.store(false);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    Manager<async> manager{rasterizer, gpu, textures, buffers, queries};
    std::atomic<unsigned> completed{};
    for (unsigned round = 0; round < 16; ++round) {
        manager.ready.store(false);
        buffers.pending = true;
        manager.SignalFence([&] { ++completed; });
        manager.SignalFence([&] { ++completed; });
        assert(completed == round * 3);
        manager.SignalGpu();
        manager.WaitForFence();
        assert(completed == round * 3 + 2);
        // The dependency must disappear after retirement, without a frame tick.
        manager.SignalFence([&] { ++completed; });
        assert(completed == round * 3 + 3);
    }
    assert(manager.host_fences == 16 && rasterizer.flushes == 16);
}

void CheckInFlightPublication(bool exact_readback) {
    Settings::exact_readback.store(exact_readback);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    RetirementGate gpu_gate, copy_gate, callback_gate;
    Manager<true> manager{rasterizer, gpu, textures, buffers, queries};
    manager.on_wait = [&] { gpu_gate.StopOnce(); };
    queries.before_pop = [&] { copy_gate.StopOnce(); };
    queries.pending = true;
    std::atomic<unsigned> completed{};
    manager.SignalFence([&] {
        assert(queries.result.load(std::memory_order_acquire) == 42);
        callback_gate.StopOnce();
        assert(completed.fetch_add(1) == 0);
    });
    gpu_gate.AwaitEntry(); // The readback has left the queue but is not ready.
    manager.SignalFence([&] { assert(completed.fetch_add(1) == 1); });
    assert(completed == 0);
    manager.SignalGpu();
    gpu_gate.released.release();
    copy_gate.AwaitEntry(); // GPU completion alone must not publish guest completion.
    manager.SignalFence([&] { assert(completed.fetch_add(1) == 2); });
    assert(completed == 0);
    copy_gate.released.release();
    callback_gate.AwaitEntry(); // Nor may a later signal overtake a running callback.
    manager.SignalFence([&] { assert(completed.fetch_add(1) == 3); });
    assert(completed == 0);
    callback_gate.released.release();
    manager.WaitForFence();
    assert(completed == 4 && manager.host_fences == 1 && rasterizer.flushes == 1);
}

void CheckForcedCallback(bool exact_readback) {
    Settings::exact_readback.store(exact_readback);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    RetirementGate gate;
    Manager<true> manager{rasterizer, gpu, textures, buffers, queries};
    std::atomic<unsigned> completed{};
    manager.SignalFence([&] {
        gate.StopOnce();
        assert(completed.fetch_add(1) == 0);
    }, true);
    gate.AwaitEntry(); // A forced callback can carry ordering without GPU work.
    manager.SignalFence([&] { assert(completed.fetch_add(1) == 1); });
    assert(completed == 0);
    gate.released.release();
    manager.WaitForFence();
    assert(completed == 2 && manager.host_fences == 0 && manager.waits == 0);
}

template <bool async>
void CheckOrdering(bool exact_readback) {
    Settings::exact_readback.store(exact_readback);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    Manager<async> manager{rasterizer, gpu, textures, buffers, queries};
    std::atomic<unsigned> completed{};
    // Empty query fences must not submit or wait for the host GPU.
    manager.SignalQueryFence([&] { ++completed; });
    assert(completed == 1 && manager.host_fences == 0 && manager.waits == 0);
    assert(rasterizer.flushes == 0);

    queries.pending = true;
    manager.SignalQueryFence([&] {
        assert(queries.result.load(std::memory_order_acquire) == 42);
        assert(completed.fetch_add(1) == 1);
    });
    assert(completed == 1); // A pending GPU readback must never publish early.
    manager.SignalQueryFence([&] { assert(completed.fetch_add(1) == 2); });
    assert(completed == 1); // A later stub cannot overtake an in-flight fence.
    assert(manager.host_fences == 1 && rasterizer.flushes == 1);
    // Changing the live setting must not alter retirement of already queued queries.
    Settings::exact_readback.store(!exact_readback);
    manager.SignalGpu();
    manager.WaitForFence();
    Settings::exact_readback.store(exact_readback);
    assert(completed == 3);
    assert(textures.retired == buffers.retired && buffers.retired == queries.retired);

    // Each source of a readback requires the same ordering, not just query banks.
    for (Cache* cache : {&buffers, &textures}) {
        manager.ready.store(false);
        cache->pending = true;
        const unsigned before = completed;
        manager.SignalQueryFence([&] { ++completed; });
        assert(completed == before);
        manager.SignalGpu();
        manager.WaitForFence();
        assert(completed == before + 1);
    }
    assert(manager.host_fences == 3);
}

// Ordinary completion signals, including syncpoints, must also respect pending
// readbacks. Exercise the real queue with both synchronous and worker retirement.
template <bool async>
void CheckOrdinaryFenceOrdering(bool exact_readback) {
    Settings::exact_readback.store(exact_readback);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    Manager<async> manager{rasterizer, gpu, textures, buffers, queries};
    std::atomic<unsigned> completed{};
    buffers.pending = true;
    manager.SignalFence([&] {
        assert(buffers.result.load(std::memory_order_acquire) == 42);
        assert(completed.fetch_add(1) == 0);
    });
    assert(completed == 0);
    // This empty signal must not overtake the queued or in-flight readback.
    manager.SignalFence([&] { assert(completed.fetch_add(1) == 1); });
    assert(completed == 0);
    manager.SignalGpu();
    manager.WaitForFence();
    assert(completed == 2);
    assert(manager.host_fences == 1 && rasterizer.flushes == 1);
}

// Where a CPU access to pending readback data faults until publication, an ordinary
// signal need not wait for the readback, but the readback itself still retires.
template <bool async>
void CheckProtectedReadback(bool exact_readback) {
    Settings::exact_readback.store(exact_readback);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    Manager<async> manager{rasterizer, gpu, textures, buffers, queries};
    std::atomic<unsigned> completed{};
    buffers.protects_pending = true;
    buffers.pending = true;
    manager.SignalFence([&] { ++completed; });
    assert(manager.host_fences == 1 && rasterizer.flushes == 1);
    manager.SignalFence([&] { ++completed; });
    assert(completed == (exact_readback ? 0u : 2u));
    manager.SignalGpu();
    manager.WaitForFence();
    assert(completed == 2 && buffers.result.load(std::memory_order_acquire) == 42);
    // Query fences keep their slots alive until retirement in every mode.
    manager.ready.store(false);
    buffers.pending = true;
    manager.SignalQueryFence([&] { ++completed; });
    assert(completed == 2);
    manager.SignalGpu();
    manager.WaitForFence();
    assert(completed == 3);
}

void BenchmarkEmpty() {
    Settings::exact_readback.store(false);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    Manager<true> manager{rasterizer, gpu, textures, buffers, queries};
    constexpr unsigned count = 100000;
    unsigned completed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < count; ++i) manager.SignalQueryFence([&] { ++completed; });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    assert(completed == count && manager.host_fences == 0 && manager.waits == 0);
    std::printf("Empty query fences: %.1f ns/call, zero GPU submits/waits\n",
        std::chrono::duration<double, std::nano>(elapsed).count() / count);
}

void BenchmarkOrdinaryBacklog() {
    Settings::exact_readback.store(false);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    RetirementGate gate;
    Manager<true> manager{rasterizer, gpu, textures, buffers, queries};
    queries.before_pop = [&] { gate.StopOnce(); };
    queries.on_commit = [&] { manager.SyncOperation([] {}); };
    manager.SignalReference();
    gate.AwaitEntry();
    constexpr unsigned count = 10000;
    std::atomic<unsigned> completed{};
    const auto start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < count; ++i) manager.SignalFence([&] { ++completed; });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    std::printf("Ordinary backlog: %.1f ns/call, %u/%u inline, %u GPU submits/waits\n",
        std::chrono::duration<double, std::nano>(elapsed).count() / count,
        completed.load(), count, manager.host_fences.load() + manager.waits.load());
    gate.released.release();
    manager.WaitForFence();
    assert(completed == count);
}

template <bool async>
void CheckCheckpointReadback(bool exact_readback) {
    Settings::exact_readback.store(exact_readback);
    VideoCore::RasterizerInterface rasterizer;
    Tegra::GPU gpu;
    Cache textures, buffers, queries;
    Manager<async> manager{rasterizer, gpu, textures, buffers, queries};
    std::atomic<bool> completed{};

    // A long-running counter can create its first readback while preparing the
    // flush, without a guest counter report or previously pending query data.
    queries.pending_on_presync = true;
    assert(!queries.HasUncommittedFlushes());
    manager.SignalQueryFence([&] {
        assert(queries.result.load(std::memory_order_acquire) == 42);
        completed.store(true, std::memory_order_release);
    });
    assert(manager.host_fences == 1 && rasterizer.flushes == 1);
    assert(!completed.load(std::memory_order_acquire));
    manager.SignalGpu();
    manager.WaitForFence();
    assert(completed.load(std::memory_order_acquire));
    assert(textures.retired == buffers.retired && buffers.retired == queries.retired);
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--benchmark") {
        BenchmarkEmpty();
        BenchmarkOrdinaryBacklog();
        return 0;
    }
    CheckNormalBacklog();
    CheckQueuedReadback();
    CheckHighBacklog();
    CheckNormalResumes<true>();
    CheckNormalResumes<false>();
    for (bool exact_readback : {false, true}) {
        CheckInFlightPublication(exact_readback);
        CheckForcedCallback(exact_readback);
        CheckOrdering<true>(exact_readback);
        CheckOrdering<false>(exact_readback);
        CheckOrdinaryFenceOrdering<true>(exact_readback);
        CheckOrdinaryFenceOrdering<false>(exact_readback);
        CheckCheckpointReadback<true>(exact_readback);
        CheckCheckpointReadback<false>(exact_readback);
        CheckProtectedReadback<true>(exact_readback);
        CheckProtectedReadback<false>(exact_readback);
    }
    BenchmarkEmpty();
    std::puts("PASS: GPU readback query fence ordering and empty fast path");
}
