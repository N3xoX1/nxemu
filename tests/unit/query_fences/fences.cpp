// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <chrono>
#include <cstdio>
#include <future>
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
    bool HasUncommittedFlushes() const { return pending; }
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
        std::scoped_lock lock(slot_guard);
        slots.push_back(std::exchange(pending, false));
    }
    void PopAsyncFlushes() {
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
        ready.wait(false, std::memory_order_acquire);
    }
};

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

int main() {
    for (bool exact_readback : {false, true}) {
        CheckOrdering<true>(exact_readback);
        CheckOrdering<false>(exact_readback);
        CheckCheckpointReadback<true>(exact_readback);
        CheckCheckpointReadback<false>(exact_readback);
    }
    BenchmarkEmpty();
    std::puts("PASS: GPU readback query fence ordering and empty fast path");
}
