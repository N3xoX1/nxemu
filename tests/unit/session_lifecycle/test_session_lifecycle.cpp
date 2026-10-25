// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>

#include "nxemu-os/core/hle/kernel/host_thread_state.h"
#include "yuzu_video_core/host1x/syncpoint_manager.h"

using namespace std::chrono_literals;

struct Thread {
    int& kernel;
    static inline int destroyed{};
    ~Thread() { ++destroyed; }
};

static void TestHostThreadSessions() {
    Kernel::HostThreadState<Thread> state;
    int kernel = 1;
    int created = 0;
    const auto factory = [&] {
        ++created;
        return std::make_unique<Thread>(kernel);
    };
    auto* dummy = state.Get(1, factory);
    assert(&dummy->kernel == &kernel && created == 1);
    assert(state.Get(1, factory) == dummy && created == 1);
    Thread guest{kernel};
    state.Set(1, &guest);
    assert(state.Get(1, factory) == &guest);
    state.Set(1, nullptr);
    assert(state.Get(1, factory) == dummy);

    // A new session can reuse the exact same kernel address and host thread.
    kernel = 2;
    state.Set(1, &guest);
    dummy = state.Get(2, factory);
    assert(dummy != &guest && created == 2 && Thread::destroyed == 1);
    assert(dummy->kernel == 2);
    // Explicit registration must replace an already cached fallback thread.
    state.Set(2, &guest);
    assert(state.Get(2, factory) == &guest);
    // Registration on a fresh session must not allocate a fallback.
    state.Set(3, &guest);
    assert(state.Get(3, factory) == &guest && created == 2);
    assert(Thread::destroyed == 2);
}

static void TestFenceCancellation() {
    auto manager = std::make_unique<Tegra::Host1x::SyncpointManager>();
    std::stop_source already_stopped;
    already_stopped.request_stop();
    assert(!manager->WaitHost(0, 1, already_stopped.get_token()));
    manager->IncrementHost(0); // No callback may retain the cancelled wait's stack.
    assert(manager->WaitHost(0, 1, std::stop_token{}));

    for (int i = 0; i < 300; ++i) {
        const auto target = manager->GetHostSyncpointValue(0) + 1;
        std::stop_source stop;
        std::promise<void> started;
        auto wait = std::async(std::launch::async, [&] {
            started.set_value();
            return manager->WaitHost(0, target, stop.get_token());
        });
        started.get_future().wait();
        // Include shutdown while the GPU never reaches the requested fence.
        if (i % 3 == 0) {
            assert(wait.wait_for(1ms) == std::future_status::timeout);
            stop.request_stop();
            assert(wait.wait_for(2s) == std::future_status::ready);
            assert(!wait.get());
            manager->IncrementHost(0);
        } else if (i % 3 == 1) {
            manager->IncrementHost(0);
            assert(wait.wait_for(2s) == std::future_status::ready);
            assert(wait.get());
        } else {
            // Completion and cancellation may arrive together.
            std::jthread cancel([&] { stop.request_stop(); });
            manager->IncrementHost(0);
            assert(wait.wait_for(2s) == std::future_status::ready);
            (void)wait.get();
        }
    }

    // GPU actions run under the syncpoint mutex: cancellation must not relock it.
    std::stop_source stop;
    manager->RegisterHostAction(1, 1, [&] { stop.request_stop(); });
    auto wait = std::async(std::launch::async, [&] {
        return manager->WaitHost(1, 2, stop.get_token());
    });
    assert(wait.wait_for(1ms) == std::future_status::timeout);
    manager->IncrementHost(1);
    assert(wait.wait_for(2s) == std::future_status::ready);
    assert(!wait.get());
    manager->IncrementHost(1);
}

int main() {
    // Bound hangs even when they occur inside a mutex or future destructor.
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::jthread watchdog([&] {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, 20s, [&] { return done; })) {
            std::fputs("Session lifecycle test timed out\n", stderr);
            std::abort();
        }
    });
    TestHostThreadSessions();
    TestFenceCancellation();
    {
        std::lock_guard lock(mutex);
        done = true;
    }
    cv.notify_all();
    std::puts("PASS: host-thread session lifetime and GPU fence shutdown (300 races)");
}
