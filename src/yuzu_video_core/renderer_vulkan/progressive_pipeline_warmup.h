// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "yuzu_video_core/renderer_vulkan/native_pipeline_build_queue.h"

namespace Vulkan {

// Owns admission/synchronization only. Compiler inputs and completed Vulkan
// objects belong to the cache owner, which publishes them on its GPU thread.
// One speculative request at a time, sharing the runtime compiler limit.
class ProgressivePipelineWarmup {
public:
    enum class DemandResult { Runtime, Pending, Complete };
    using Clock = std::chrono::steady_clock;

    // finished_: called once per job that leaves the queue (built, failed, or taken by the runtime).
    ProgressivePipelineWarmup(NativePipelineBuildQueue& queue_, size_t count,
                              std::function<bool(size_t)> build_, std::function<void()> finished_ = {})
        : queue(queue_), build(std::move(build_)), finished(std::move(finished_)), jobs(count) {
        completed.reserve(count); // Completion cannot allocate on a compiler worker.
        admission = std::jthread([this](std::stop_token stop) { Run(stop); });
    }
    ~ProgressivePipelineWarmup() {
        Stop();
        queue.WaitForRequests(); // No callback can outlive its build closure.
    }
    ProgressivePipelineWarmup(const ProgressivePipelineWarmup&) = delete;
    ProgressivePipelineWarmup& operator=(const ProgressivePipelineWarmup&) = delete;

    void Stop() {
        admission.request_stop();
        changed.notify_all();
        if (admission.joinable()) admission.join();
        std::scoped_lock lock(mutex);
        for (auto& job : jobs) {
            if (job.status == Status::Running && queue.Cancel(job.request))
                job.status = Status::Cancelled;
        }
        changed.notify_all();
    }

    void RecordFrame(Clock::time_point now = Clock::now()) {
        std::scoped_lock lock(mutex);
        if (last_frame != Clock::time_point{} && now - last_frame > std::chrono::milliseconds(45))
            pause_until = std::max(pause_until, now + std::chrono::milliseconds(150));
        last_frame = now;
        // Admission already wakes at its previous deadline; extending it needs no notification.
    }

    template<class Publish>
    void PublishReady(Publish publish) {
        if (!has_ready.load(std::memory_order_acquire)) return;
        std::scoped_lock lock(mutex);
        for (const auto index : completed) {
            if (jobs[index].status != Status::Ready) continue;
            publish(index);
            jobs[index].status = Status::Published;
        }
        completed.clear();
        has_ready.store(false, std::memory_order_release);
    }

    DemandResult Demand(size_t index, bool allow_skip) {
        std::unique_lock lock(mutex);
        auto& job = jobs.at(index);
        if (job.status == Status::Queued) {
            job.status = Status::Cancelled; // The runtime now owns this exact key.
            if (finished) finished();
            return DemandResult::Runtime;
        }
        if (job.status == Status::Running) {
            changed.wait(lock, [&] { return job.status != Status::Running || job.request; });
            if (job.status == Status::Running) {
                const auto request = job.request;
                lock.unlock();
                queue.Promote(request);
                if (allow_skip) return DemandResult::Pending;
                queue.RunNow(request);
                lock.lock();
                changed.wait(lock, [&] { return job.status != Status::Running; });
            }
        }
        return job.status == Status::Ready || job.status == Status::Published
                   ? DemandResult::Complete : DemandResult::Runtime;
    }

private:
    enum class Status { Queued, Running, Ready, Failed, Cancelled, Published };
    struct Job {
        Status status{Status::Queued};
        NativePipelineBuildQueue::Handle request;
    };
    void Build(size_t index) noexcept {
        bool success{};
        try { success = build(index); } catch (...) { /* Runtime retries failures. */ }
        {
            std::scoped_lock lock(mutex);
            jobs[index].status = success ? Status::Ready : Status::Failed;
            if (success) {
                completed.push_back(index);
                has_ready.store(true, std::memory_order_release);
            }
        }
        changed.notify_all();
        if (finished) finished();
    }
    void Run(std::stop_token stop) {
        for (size_t i = 0; i < jobs.size(); ++i) {
            std::unique_lock lock(mutex);
            while (!stop.stop_requested() && pause_until > Clock::now())
                changed.wait_until(lock, pause_until);
            if (stop.stop_requested()) return;
            if (jobs[i].status != Status::Queued) continue;
            jobs[i].status = Status::Running;
            lock.unlock();
            NativePipelineBuildQueue::Handle request;
            try {
                request = queue.QueueWork([this, i] { Build(i); }, false);
            } catch (...) {
                lock.lock();
                jobs[i].status = Status::Failed;
                changed.notify_all();
                lock.unlock();
                if (finished) finished();
                continue;
            }
            lock.lock();
            jobs[i].request = std::move(request);
            changed.notify_all();
            changed.wait(lock, [&] { return stop.stop_requested() || jobs[i].status != Status::Running; });
        }
    }

    NativePipelineBuildQueue& queue;
    std::function<bool(size_t)> build;
    std::function<void()> finished;
    std::vector<Job> jobs;
    std::vector<size_t> completed;
    std::atomic<bool> has_ready{};
    std::mutex mutex;
    std::condition_variable changed;
    Clock::time_point last_frame{}, pause_until{};
    std::jthread admission;
};
} // namespace Vulkan
