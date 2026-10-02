// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstring>
#include <span>
#include <vector>
#include <boost/container/small_vector.hpp>
#include "yuzu_common/common_types.h"
#include "yuzu_common/scratch_buffer.h"

namespace Service {

// Response buffers are private until the reply transaction accepts the request.
// The CMIF serializer transfers its existing scratch allocation, avoiding an
// additional copy/allocation for large responses.
class IpcResponseWrites {
    struct Write {
        u64 address;
        Common::ScratchBuffer<u8> data;
    };
public:
    void Add(u64 address, const void* data, size_t size) {
        Common::ScratchBuffer<u8> copy(size);
        std::memcpy(copy.data(), data, size);
        Add(address, std::move(copy));
    }
    void Add(u64 address, Common::ScratchBuffer<u8>&& data) {
        writes.push_back({address, std::move(data)});
    }
    template <typename Publish>
    void PublishTo(Publish&& publish) {
        for (const auto& write : writes)
            publish(write.address, std::span<const u8>{write.data.data(), write.data.size()});
        writes.clear();
    }
    void Clear() { writes.clear(); }
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
