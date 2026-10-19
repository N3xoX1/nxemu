// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace VideoCommon {

// Charges snapshot payloads until the last owner releases them, including descriptor
// tables retained by the reuse cache. Allocation/control-block overhead is not included.
class SnapshotMemoryBudget {
    struct State {
        explicit State(size_t limit_) : limit(limit_) {}
        const size_t limit;
        std::atomic_size_t used{};
    };

    struct Charge {
        Charge(std::shared_ptr<State> state_, size_t bytes_)
            : state(std::move(state_)), bytes(bytes_) {}
        Charge(Charge&&) noexcept = default;
        Charge(const Charge&) = delete;
        ~Charge() {
            if (state) {
                state->used.fetch_sub(bytes, std::memory_order_relaxed);
            }
        }
        std::shared_ptr<State> state;
        size_t bytes;
    };

    template <class T>
    struct Allocation {
        template <class... Args>
        Allocation(Charge charge_, Args&&... args)
            : charge(std::move(charge_)), value(std::forward<Args>(args)...) {}
        // Destruction frees the payload before returning its budget.
        Charge charge;
        T value;
    };

public:
    explicit SnapshotMemoryBudget(size_t limit) : state(std::make_shared<State>(limit)) {}

    [[nodiscard]] size_t Used() const noexcept {
        return state->used.load(std::memory_order_relaxed);
    }

    // Reserve before constructing/allocating the payload. A failed construction returns
    // the charge automatically; a full budget lets the caller use synchronous translation.
    template <class T, class... Args>
    [[nodiscard]] std::shared_ptr<T> TryMakeShared(size_t bytes, Args&&... args) const {
        size_t used = Used();
        do {
            if (bytes > state->limit - used) {
                return {};
            }
        } while (!state->used.compare_exchange_weak(used, used + bytes,
                                                    std::memory_order_relaxed));
        Charge charge{state, bytes};
        auto allocation = std::make_shared<Allocation<T>>(std::move(charge),
                                                          std::forward<Args>(args)...);
        T* value = &allocation->value;
        return {std::move(allocation), value};
    }

private:
    std::shared_ptr<State> state;
};

} // namespace VideoCommon
