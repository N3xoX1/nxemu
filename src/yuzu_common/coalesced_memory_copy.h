// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Common {

// Merge only spans contiguous in BOTH host pointers. Holes and unrelated physical
// pages must flush the pending run before they are processed by the caller.
class CoalescedMemoryCopy {
public:
    bool Append(void* dst, const void* src, std::size_t size) {
        if (!size) {
            return false;
        }
        const auto dst_address = reinterpret_cast<std::uintptr_t>(dst);
        const auto src_address = reinterpret_cast<std::uintptr_t>(src);
        bool flushed = false;
        if (pending_size &&
            (dst_address - reinterpret_cast<std::uintptr_t>(pending_dst) != pending_size ||
             src_address - reinterpret_cast<std::uintptr_t>(pending_src) != pending_size)) {
            flushed = Flush();
        }
        if (!pending_size) {
            pending_dst = dst;
            pending_src = src;
        }
        pending_size += size;
        return flushed;
    }

    bool Flush() {
        if (!pending_size) {
            return false;
        }
        std::memcpy(pending_dst, pending_src, pending_size);
        pending_size = 0;
        return true;
    }

private:
    void* pending_dst{};
    const void* pending_src{};
    std::size_t pending_size{};
};

} // namespace Common
