// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace Vulkan {
// CPU descriptor data is read by the scheduler worker after the draw is recorded.
// Keep pending uploads stable, and drain that worker before recycling their storage.
template <typename Entry, size_t FrameCapacity, size_t Frames = 8>
class DescriptorPayload {
public:
    void TickFrame() {
        frame = (frame + 1) % Frames;
        used = 0;
        overflow_active = false;
    }

    template <typename WaitWorker>
    void Acquire(size_t count, WaitWorker&& wait_worker) {
        if (overflow_active) {
            // The ring was drained when the previous overflow upload began.
            used = 0;
            overflow_active = false;
        }
        if (count > FrameCapacity) {
            wait_worker();
            overflow.resize(count);
            upload = overflow.data();
            overflow_active = true;
        } else {
            if (count > FrameCapacity - used) {
                wait_worker();
                used = 0;
            }
            upload = ring.data() + frame * FrameCapacity + used;
            used += count;
        }
        written = 0;
        reserved = count;
    }

    void Append(const Entry& value) {
        if (written >= reserved) {
            throw std::length_error("Descriptor upload exceeded its reservation");
        }
        upload[written++] = value;
    }
    const Entry* Data() const noexcept { return upload; }
    size_t Size() const noexcept { return written; }

private:
    static_assert(FrameCapacity > 0 && Frames > 0);
    std::array<Entry, FrameCapacity * Frames> ring;
    std::vector<Entry> overflow;
    Entry* upload{};
    size_t frame{}, used{}, reserved{}, written{};
    bool overflow_active{};
};
} // namespace Vulkan
