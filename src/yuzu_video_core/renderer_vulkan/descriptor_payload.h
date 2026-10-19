// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace Vulkan {
// CPU descriptor data is read by the scheduler worker after the draw is recorded.
// Keep uploads stable until the worker has consumed them. Recycle completed blocks
// independently of frame boundaries, and bound memory when the worker falls behind.
template <typename Entry, size_t BlockCapacity, size_t MaxBlocks = 8>
class DescriptorPayload {
public:
    // retire records a completion callback after all previously recorded reads.
    template <typename WaitWorker, typename Retire>
    void Acquire(size_t count, WaitWorker&& wait_worker, Retire&& retire) {
        if (count != 0 && (current == MaxBlocks || count > blocks[current].capacity - used)) {
            if (current != MaxBlocks) {
                retire([completed = blocks[current].completed] {
                    completed->store(true, std::memory_order_release);
                });
                current = MaxBlocks;
            }
            for (size_t index = 0; index < MaxBlocks; ++index) {
                if (!blocks[index].completed ||
                    blocks[index].completed->load(std::memory_order_acquire)) {
                    current = index;
                    break;
                }
            }
            if (current == MaxBlocks) {
                wait_worker();
                current = 0;
            }
            auto& block = blocks[current];
            if (count > block.capacity) {
                const size_t capacity = count > BlockCapacity ? count : BlockCapacity;
                auto entries = std::make_unique<Entry[]>(capacity);
                block.entries = std::move(entries);
                block.capacity = capacity;
            }
            if (!block.completed) {
                block.completed = std::make_shared<std::atomic<bool>>(false);
            } else {
                block.completed->store(false, std::memory_order_relaxed);
            }
            used = 0;
        }
        upload = current == MaxBlocks ? nullptr : blocks[current].entries.get() + used;
        used += count;
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
    static_assert(BlockCapacity > 0 && MaxBlocks > 0);
    struct Block {
        std::unique_ptr<Entry[]> entries;
        size_t capacity{};
        std::shared_ptr<std::atomic<bool>> completed;
    };
    std::array<Block, MaxBlocks> blocks;
    Entry* upload{};
    size_t current{MaxBlocks}, used{}, reserved{}, written{};
};
} // namespace Vulkan
