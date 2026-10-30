#pragma once

#include <algorithm>
#include <cstring>
#include "yuzu_common/common_types.h"

namespace Core {

// Calls func(offset, size) for each contiguous run of changed 4-byte words.
// Skip unchanged 64-byte blocks, but never merge across an unchanged word: it can
// contain a GPU write that has not been downloaded to guest memory yet.
template <typename Func>
void ForEachNceWrittenRange(const u8 * current, const u8 * snapshot, u64 page_size, Func && func)
{
    constexpr u64 granularity = 64;
    constexpr u64 word = 4;
    u64 begin = page_size;
    const auto finish = [&](u64 end) {
        if (begin != page_size) {
            func(begin, end - begin);
            begin = page_size;
        }
    };
    for (u64 block = 0; block < page_size;) {
        const u64 block_size = std::min(granularity, page_size - block);
        const u64 end = block + block_size;
        if (std::memcmp(current + block, snapshot + block, block_size) == 0) {
            finish(block);
        } else {
            for (u64 offset = block; offset < end;) {
                const u64 size = std::min(word, end - offset);
                if (std::memcmp(current + offset, snapshot + offset, size) == 0) {
                    finish(offset);
                } else if (begin == page_size) {
                    begin = offset;
                }
                offset += size;
            }
        }
        block = end;
    }
    finish(page_size);
}

} // namespace Core
