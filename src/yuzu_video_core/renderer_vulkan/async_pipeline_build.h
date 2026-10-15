// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <exception>
#include <memory>
#include <utility>

namespace Vulkan {

// One producer, one owner of the result. A critical draw may claim a queued job
// itself, rather than waiting behind unrelated shaders. Completion publishes
// both successful results and failures; no exception may strand a waiter.
template <class Pipeline>
class AsyncPipelineBuild {
public:
    template <class Build>
    bool TryBuild(Build&& build) noexcept {
        auto expected = State::Queued;
        if (!state.compare_exchange_strong(expected, State::Building,
                                           std::memory_order_acq_rel)) {
            return false;
        }
        try {
            pipeline = std::forward<Build>(build)();
        } catch (...) {
            exception = std::current_exception();
        }
        state.store(State::Complete, std::memory_order_release);
        state.notify_all();
        return true;
    }

    bool IsComplete() const noexcept {
        return state.load(std::memory_order_acquire) == State::Complete;
    }

    void Wait() const noexcept {
        for (auto current = state.load(std::memory_order_acquire); current != State::Complete;
             current = state.load(std::memory_order_acquire)) {
            state.wait(current, std::memory_order_acquire);
        }
    }

    std::unique_ptr<Pipeline> Take() {
        Wait();
        if (exception) {
            std::rethrow_exception(exception);
        }
        return std::move(pipeline);
    }

private:
    enum class State { Queued, Building, Complete };
    std::atomic<State> state{State::Queued};
    std::unique_ptr<Pipeline> pipeline;
    std::exception_ptr exception;
};
} // namespace Vulkan
