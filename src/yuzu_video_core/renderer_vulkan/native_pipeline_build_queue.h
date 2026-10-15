// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <semaphore>
#include <utility>

#include "yuzu_common/scope_exit.h"
#include "yuzu_common/thread_worker.h"

namespace Vulkan {

// Runtime builds share a bounded native compiler pool. Disk loading can still
// use its larger pool without flooding gameplay with compiler threads.
class NativePipelineBuildQueue {
    struct Request {
        explicit Request(Common::UniqueFunction<void> work_) : work(std::move(work_)) {}
        Common::UniqueFunction<void> work;
    };

public:
    using Handle = std::shared_ptr<Request>;
    // This is a metadata backlog, not the number of concurrent Metal compilers.
    // A cold frame can require over 200 small passes. A 64-ticket limit blocked
    // their producer behind native compilation before it could expose the burst.
    static constexpr size_t MaxPendingBuilds = 256;

    explicit NativePipelineBuildQueue(size_t num_workers)
        : workers(num_workers, "VkRuntimeNative") {}

    ~NativePipelineBuildQueue() {
        WaitForRequests();
    }

    Handle QueueWork(Common::UniqueFunction<void> work, bool critical = true) {
        auto request = std::make_shared<Request>(std::move(work));
        slots.acquire();
        std::scoped_lock lock{requests_mutex};
        auto& requests = critical ? critical_requests : background_requests;
        try {
            requests.push_back(request);
            try {
                workers.QueueWork([this] {
                    SCOPE_EXIT { slots.release(); };
                    Handle next;
                    {
                        std::scoped_lock lock{requests_mutex};
                        auto& ready = critical_requests.empty() ? background_requests : critical_requests;
                        if (ready.empty()) {
                            // Its request was already run by RunNow().
                            return;
                        }
                        next = std::move(ready.front());
                        ready.pop_front();
                    }
                    next->work();
                });
            } catch (...) {
                // Keep the mutex until the dispatch has been accepted, so an
                // older dispatcher cannot consume this rejected request.
                requests.pop_back();
                throw;
            }
        } catch (...) {
            slots.release();
            throw;
        }
        return request;
    }

    void Promote(const Handle& request) noexcept {
        std::scoped_lock lock{requests_mutex};
        for (auto it = background_requests.begin(); it != background_requests.end(); ++it) {
            if (*it == request) {
                // Splicing does not allocate. Running/completed builds need no
                // promotion, and repeated promotion cannot duplicate a build.
                critical_requests.splice(critical_requests.end(), background_requests, it);
                return;
            }
        }
    }

    // Runs a request that no worker has started yet on the calling thread. A
    // thread that would otherwise block on this build (the command worker for a
    // critical draw or a compute dispatch) then does not wait behind unrelated
    // queued pipelines. Returns false if a worker already took the request.
    bool RunNow(const Handle& request) {
        if (!request) {
            return false;
        }
        {
            std::scoped_lock lock{requests_mutex};
            if (!Remove(critical_requests, request) && !Remove(background_requests, request)) {
                return false;
            }
        }
        // The worker dispatch queued for this request finds nothing to do and
        // releases its slot.
        request->work();
        return true;
    }

    void WaitForRequests() {
        workers.WaitForRequests();
    }

private:
    static bool Remove(std::list<Handle>& requests, const Handle& request) {
        for (auto it = requests.begin(); it != requests.end(); ++it) {
            if (*it == request) {
                requests.erase(it);
                return true;
            }
        }
        return false;
    }

    std::counting_semaphore<MaxPendingBuilds> slots{MaxPendingBuilds};
    std::mutex requests_mutex;
    std::list<Handle> critical_requests;
    std::list<Handle> background_requests;
    Common::ThreadWorker workers;
};

} // namespace Vulkan
