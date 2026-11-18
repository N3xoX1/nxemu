#include "yuzu_common/common_types.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

void Check(bool, const char*);
namespace QueueRegression {
#include "queue.inc"
struct ThreadManager {
    struct Rasterizer {
        void StopInPlaceWaits() {}
    };
    struct State { BackQueue back; };
    std::unique_ptr<State> state = std::make_unique<State>();
    Rasterizer* rasterizer = nullptr;
    std::jthread thread, decode_thread;
    void StopThread();
};
#include "stop.inc"
}

void QueueTests() {
    using namespace QueueRegression;
    ThreadManager manager;
    auto& queue = manager.state->back;
    std::recursive_mutex execution_mutex;
    queue.threaded = true;
    queue.execution_mutex = &execution_mutex;
    std::atomic<bool> wake{}, service{};
    manager.thread = std::jthread([&](std::stop_token stop) {
        std::deque<BackQueue::Work> batch;
        while (!stop.stop_requested()) {
            if (!queue.PopWait(batch, stop, wake, service)) continue;
            const auto count = batch.size();
            {
                std::scoped_lock lock{execution_mutex};
                for (auto& work : batch) work();
            }
            batch.clear();
            queue.FinishMany(count);
        }
    });
    int memory = 0, observed = 0;
    bool gpu_thread_idle = false;
    queue.Push([&] { memory = 123; });
    queue.RunExclusive([&] {
        observed = memory;
        // The GPU thread can't start queued work while the exclusive work runs
        gpu_thread_idle = queue.done_spin.load() == queue.pushed_spin.load();
    });
    Check(observed == 123, "dependent read overtook queued memory write");
    Check(gpu_thread_idle, "exclusive work ran beside queued GPU work");

    std::atomic<bool> decoder_ready{}, stopping{}, decoder_done{};
    manager.decode_thread = std::jthread([&](std::stop_token stop) {
        std::stop_callback on_stop(stop, [&] {
            stopping.store(true);
            stopping.notify_one();
        });
        decoder_ready.store(true);
        decoder_ready.notify_one();
        stopping.wait(false);
        // Work queued after shutdown was requested may be dropped (the guest memory goes
        // away), but a decode thread waiting on it must not keep the exit waiting
        queue.Push([&] { memory = 456; });
        queue.WaitIdle();
        decoder_done.store(true);
    });
    decoder_ready.wait(false);
    manager.StopThread();
    Check(decoder_done.load(), "shutdown left the decode thread waiting on GPU work");
}
