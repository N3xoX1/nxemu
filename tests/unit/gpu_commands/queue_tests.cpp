#include "yuzu_common/bounded_threadsafe_queue.h"
#include "yuzu_common/common_types.h"
#include <atomic>
#include <chrono>
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
    struct State {
        BackQueue back;
        Common::MPSCQueue<int, 4> queue;
        std::mutex write_lock;
        bool stopping{};
        std::condition_variable_any cv;
    };
    std::unique_ptr<State> state = std::make_unique<State>();
    struct Rasterizer {
        void StopInPlaceWaits() {}
    };
    Rasterizer* rasterizer{};
    std::jthread thread, decode_thread;
    void StopThread();
};
#include "stop.inc"
}

void QueueTests() {
    using namespace QueueRegression;
    ThreadManager manager;
    auto& queue = manager.state->back;
    queue.threaded = true;
    std::atomic<bool> wake{}, service{};
    manager.thread = std::jthread([&](std::stop_token stop) {
        std::deque<BackQueue::Work> batch;
        while (!stop.stop_requested()) {
            if (!queue.PopWait(batch, stop, wake, service)) continue;
            const auto count = batch.size();
            for (auto& work : batch) work();
            batch.clear();
            queue.FinishMany(count);
        }
    });
    int memory = 0, observed = 0;
    std::thread::id execution_thread;
    queue.Push([&] { memory = 123; });
    queue.RunExclusive([&] {
        observed = memory;
        execution_thread = std::this_thread::get_id();
    });
    Check(observed == 123, "dependent read overtook queued memory write");
    Check(execution_thread == manager.thread.get_id(), "renderer work ran on decode thread");

    std::atomic<bool> decoder_ready{}, stopping{}, fence_ready{};
    manager.decode_thread = std::jthread([&](std::stop_token stop) {
        std::stop_callback on_stop(stop, [&] {
            stopping.store(true);
            stopping.notify_one();
        });
        decoder_ready.store(true);
        decoder_ready.notify_one();
        stopping.wait(false);
        // Finish an in-flight decode command after shutdown was requested. Its fence
        // must still be submitted by the execution thread before decoder join completes.
        queue.Push([&] {
            fence_ready.store(true);
            fence_ready.notify_one();
        });
        fence_ready.wait(false);
        queue.Push([&] { memory = 456; });
        queue.WaitIdle();
    });
    decoder_ready.wait(false);
    manager.StopThread();
    Check(fence_ready.load() && memory == 456, "shutdown dropped decoder completion work");

    // Exercise producer cancellation when the command queue is full at shutdown.
    ThreadManager full_queue_manager;
    for (int i = 0; i < 4; ++i) {
        Check(full_queue_manager.state->queue.TryEmplace(i), "queue should accept up to capacity");
    }
    Check(!full_queue_manager.state->queue.TryEmplace(4), "queue should report full at capacity");
    std::atomic<bool> producer_started{};
    std::atomic<bool> producer_result{true};
    std::jthread blocked_producer([&] {
        std::scoped_lock lk{full_queue_manager.state->write_lock};
        producer_started.store(true, std::memory_order_release);
        producer_started.notify_one();
        producer_result.store(full_queue_manager.state->queue.EmplaceWait(99),
                              std::memory_order_release);
    });
    producer_started.wait(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    full_queue_manager.StopThread();
    blocked_producer.join();
    Check(!producer_result.load(std::memory_order_acquire),
          "StopThread must cancel a producer blocked on a full queue");
    Check(!full_queue_manager.state->queue.EmplaceWait(100),
          "closed queue must reject subsequent EmplaceWait calls immediately");
}
