// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "yuzu_video_core/framebuffer_config.h"

namespace VideoCommon {

// Own the frame until all acquire fences have fired and the renderer has consumed it.
class CompositeRequest {
public:
    CompositeRequest(std::vector<Tegra::FramebufferConfig>&& layers_, size_t num_fences)
        : layers{std::move(layers_)}, remaining_fences{num_fences} {}

    // Fence callbacks only mark readiness. The GPU worker consumes the frame separately.
    bool Signal() {
        std::scoped_lock lk{mutex};
        if (completed || cancelled || ready) {
            return false;
        }
        if (remaining_fences != 0 && --remaining_fences != 0) {
            return false;
        }
        ready = true;
        return true;
    }

    template <typename Func>
    bool Composite(Func&& composite) {
        std::unique_lock lk{mutex};
        if (!ready || completed || cancelled || running) {
            return completed || (cancelled && !running);
        }
        running = true;
        lk.unlock();
        try {
            std::forward<Func>(composite)(layers);
        } catch (...) {
            lk.lock();
            running = false;
            cancelled = true;
            lk.unlock();
            cv.notify_all();
            throw;
        }
        lk.lock();
        running = false;
        completed = true;
        lk.unlock();
        cv.notify_all();
        return true;
    }

    void Wait() {
        std::unique_lock lk{mutex};
        cv.wait(lk, [this] { return completed || (cancelled && !running); });
    }

    void RequestCancel() {
        {
            std::scoped_lock lk{mutex};
            cancelled = true;
        }
        cv.notify_all();
    }

    void Cancel() {
        RequestCancel();
        // Asking for cancellation must never release a buffer under a running renderer.
        Wait();
    }

    bool IsCancelled() {
        std::scoped_lock lk{mutex};
        return cancelled;
    }

private:
    std::vector<Tegra::FramebufferConfig> layers;
    size_t remaining_fences;
    std::mutex mutex;
    std::condition_variable cv;
    bool running{};
    bool ready{};
    bool completed{};
    bool cancelled{};
};

// Serialize producers without preventing shutdown from cancelling an unsignaled frame.
class CompositeRequestQueue {
public:
    template <typename Func>
    void Submit(std::vector<Tegra::FramebufferConfig>&& layers, size_t num_fences, Func&& enqueue) {
        std::scoped_lock submission_lock{submission_mutex};
        Wait();
        std::shared_ptr<CompositeRequest> request;
        {
            std::scoped_lock lk{mutex};
            if (closed) {
                return;
            }
            request = std::make_shared<CompositeRequest>(std::move(layers), num_fences);
            pending = request;
            has_pending.store(true, std::memory_order_release);
        }
        std::forward<Func>(enqueue)(request);
    }

    template <typename Func>
    void Composite(Func&& composite) {
        const auto request = Pending();
        if (!request) {
            return;
        }

        try {
            if (request->Composite(std::forward<Func>(composite))) {
                ClearPending(request);
            }
        } catch (...) {
            ClearPending(request);
            throw;
        }
    }

    void Wait() {
        if (!has_pending.load(std::memory_order_acquire)) {
            return;
        }

        if (const auto request = Pending()) {
            request->Wait();
            ClearPending(request);
        }
    }

    void CancelPending() {
        // Unblock a waiting producer, then cancel its current request under submission_mutex.
        if (auto request = Pending()) {
            request->RequestCancel();
        }
        std::scoped_lock submission_lock{submission_mutex};
        if (auto request = Pending()) {
            request->Cancel();
            ClearPending(request);
        }
    }

    void RequestCancel() {
        std::shared_ptr<CompositeRequest> request;
        {
            std::scoped_lock lk{mutex};
            closed = true;
            request = pending;
        }
        if (request) {
            request->RequestCancel();
        }
    }

    void Cancel() {
        RequestCancel();
        Wait();
        // A producer may still be inside enqueue after its request was cancelled.
        // Destruction must also wait for that transaction to leave the GPU interface.
        std::scoped_lock submission_lock{submission_mutex};
    }

private:
    std::shared_ptr<CompositeRequest> Pending() {
        std::scoped_lock lk{mutex};
        return pending;
    }

    void ClearPending(const std::shared_ptr<CompositeRequest>& request) {
        std::scoped_lock lk{mutex};
        if (pending == request) {
            pending.reset();
            has_pending.store(false, std::memory_order_release);
        }
    }

    std::mutex submission_mutex;
    std::mutex mutex;
    std::shared_ptr<CompositeRequest> pending;
    std::atomic_bool has_pending{};
    bool closed{};
};

} // namespace VideoCommon
