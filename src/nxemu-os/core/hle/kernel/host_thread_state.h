// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <utility>

namespace Kernel {

// Host threads can outlive a kernel session, including reuse at the same address.
// Keep both the fallback thread and the current guest thread within that session.
template <typename Thread>
class HostThreadState {
public:
    template <typename Factory>
    Thread* Get(std::uint64_t session, Factory&& factory) {
        Bind(session);
        if (!current) {
            if (!dummy) {
                dummy = factory();
            }
            current = dummy.get();
        }
        return current;
    }

    void Set(std::uint64_t session, Thread* thread) {
        Bind(session);
        current = thread;
    }

private:
    void Bind(std::uint64_t session) {
        if (generation != session) {
            current = nullptr;
            dummy.reset();
            generation = session;
        }
    }

    std::uint64_t generation{};
    std::unique_ptr<Thread> dummy;
    Thread* current{};
};

} // namespace Kernel
