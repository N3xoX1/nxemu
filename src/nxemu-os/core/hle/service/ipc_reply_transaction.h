// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstring>
#include <algorithm>
#include <array>
#include <memory>
#include <mutex>
#include <span>
#include <vector>
#include <boost/container/small_vector.hpp>
#include "yuzu_common/common_types.h"
#include "yuzu_common/scratch_buffer.h"

namespace Service {

// Ownership returns here only after publication or cancellation. A pending
// reply retains the pool, so service destruction cannot invalidate its buffer.
class IpcResponseBufferPool {
public:
    Common::ScratchBuffer<u8> Acquire(size_t size) {
        Common::ScratchBuffer<u8> buffer;
        {
            std::scoped_lock lock{mutex};
            size_t best = buffers.size();
            for (size_t i = 0; i < buffers.size(); ++i) {
                if (buffers[i].capacity() >= size &&
                    (best == buffers.size() || buffers[i].capacity() < buffers[best].capacity()))
                    best = i;
            }
            // Reuse and grow an undersized allocation rather than leaving
            // every idle slot occupied by buffers that no longer fit.
            if (best == buffers.size()) {
                for (size_t i = 0; i < buffers.size(); ++i) {
                    if (buffers[i].capacity() &&
                        (best == buffers.size() || buffers[i].capacity() > buffers[best].capacity()))
                        best = i;
                }
            }
            if (best != buffers.size()) buffer = std::move(buffers[best]);
        }
        buffer.resize_destructive(size);
        return buffer;
    }

    void Recycle(Common::ScratchBuffer<u8>&& buffer) {
        // At most 8 MiB idle storage per service; large exceptional responses
        // and more than four simultaneous replies are released normally.
        if (!buffer.capacity() || buffer.capacity() > 2 * 1024 * 1024) return;
        std::scoped_lock lock{mutex};
        for (auto& slot : buffers) {
            if (!slot.capacity()) {
                slot = std::move(buffer);
                return;
            }
        }
        auto smallest = std::min_element(buffers.begin(), buffers.end(),
            [](const auto& left, const auto& right) { return left.capacity() < right.capacity(); });
        if (smallest->capacity() < buffer.capacity()) *smallest = std::move(buffer);
    }
private:
    std::mutex mutex;
    std::array<Common::ScratchBuffer<u8>, 4> buffers;
};

// Response buffers are private until the reply transaction accepts the request.
// The CMIF serializer transfers its existing scratch allocation, avoiding an
// additional copy/allocation for large responses.
class IpcResponseWrites {
    struct Write {
        u64 address;
        Common::ScratchBuffer<u8> data;
        std::shared_ptr<IpcResponseBufferPool> pool;
    };
public:
    ~IpcResponseWrites() { Clear(); }
    void Add(u64 address, const void* data, size_t size) {
        Common::ScratchBuffer<u8> copy(size);
        std::memcpy(copy.data(), data, size);
        Add(address, std::move(copy));
    }
    void Add(u64 address, Common::ScratchBuffer<u8>&& data,
             const std::shared_ptr<IpcResponseBufferPool>& pool = {}) {
        writes.push_back({address, std::move(data), pool});
    }
    template <typename Publish>
    bool PublishTo(Publish&& publish) {
        for (const auto& write : writes) {
            if (!publish(write.address, std::span<const u8>{write.data.data(), write.data.size()})) {
                Clear();
                return false;
            }
        }
        Clear();
        return true;
    }
    void Clear() {
        for (auto& write : writes) {
            if (write.pool) write.pool->Recycle(std::move(write.data));
        }
        writes.clear();
    }
private:
    boost::container::small_vector<Write, 2> writes;
};

// Any failed handle translation rolls back the entire set. Reserve tracking
// storage before creating handles so a tracking allocation cannot leak one.
template <typename Table>
class IpcHandleRollback {
public:
    IpcHandleRollback(Table& table_, size_t count) : table{table_} { handles.reserve(count); }
    ~IpcHandleRollback() { if (!committed) for (const auto handle : handles) table.Remove(handle); }
    IpcHandleRollback(const IpcHandleRollback&) = delete;
    IpcHandleRollback& operator=(const IpcHandleRollback&) = delete;
    void Track(u32 handle) { handles.push_back(handle); }
    void Commit() { committed = true; }
private:
    Table& table;
    std::vector<u32> handles;
    bool committed{};
};
} // namespace Service
