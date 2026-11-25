#pragma once

#include <array>
#include <cstddef>
#include "yuzu_common/common_types.h"

namespace Core {

// Under NCE, a CPU filling GPU-cached memory faults once per protected page. These streams
// recognize a thread that keeps writing the next protected page, so the pages after it can
// be invalidated before it reaches them. A thread often fills a few buffers in turn
// (vertices, indices, uniforms), so a few streams are kept, replaced in turn.
// Plain data with constant initialization: safe as a thread_local read in a signal handler.
template <size_t StreamCount = 4>
class NceWriteStreams {
public:
    static constexpr u64 FirstWindowPages = 4;
    static constexpr u64 LaterWindowPages = 16;
    // How far past the invalidated pages the next protected page is looked for: the
    // writer goes through pages already writable without faulting
    static constexpr u64 MaxWritablePages = 64;

    struct Fault {
        size_t stream;
        // Pages to invalidate from the faulting one, itself included: 1 for no coalescing
        u64 window_pages;
    };

    // A write faulted on the page at page_addr
    constexpr Fault Begin(u64 page_addr) {
        for (size_t i = 0; i < StreamCount; ++i) {
            Stream& stream = streams[i];
            if (stream.next != 0 && stream.next == page_addr) {
                ++stream.streak;
                return {i, stream.streak >= 2 ? LaterWindowPages : FirstWindowPages};
            }
        }
        const size_t i = replace;
        replace = (replace + 1) % StreamCount;
        streams[i] = {};
        return {i, 1};
    }

    // next_page: where the stream's next fault is expected, the first page after the
    // invalidated ones that is still protected
    constexpr void End(const Fault& fault, u64 next_page) {
        streams[fault.stream].next = next_page;
    }

private:
    struct Stream {
        u64 next;
        u32 streak;
    };
    std::array<Stream, StreamCount> streams{};
    size_t replace{};
};

// First page in [begin, end) for which is_protected(page) is true, or end. A sequential
// writer goes through pages that are already writable without faulting, and faults next there.
template <typename IsProtected>
constexpr u64 NextProtectedPage(u64 begin, u64 end, u64 page_size, IsProtected&& is_protected) {
    for (u64 page = begin; page < end; page += page_size) {
        if (is_protected(page)) {
            return page;
        }
    }
    return end;
}

} // namespace Core
