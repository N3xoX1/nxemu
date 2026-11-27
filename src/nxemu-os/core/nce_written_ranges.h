#pragma once

#include <algorithm>
#include <cstring>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include "yuzu_common/common_types.h"

namespace Core {

namespace nce_detail {
inline bool EqualBlock64(const u8 * a, const u8 * b) noexcept {
#if defined(__aarch64__)
    const uint8x16_t d0 = veorq_u8(vld1q_u8(a + 0), vld1q_u8(b + 0));
    const uint8x16_t d1 = veorq_u8(vld1q_u8(a + 16), vld1q_u8(b + 16));
    const uint8x16_t d2 = veorq_u8(vld1q_u8(a + 32), vld1q_u8(b + 32));
    const uint8x16_t d3 = veorq_u8(vld1q_u8(a + 48), vld1q_u8(b + 48));
    const uint8x16_t diff = vorrq_u8(vorrq_u8(d0, d1), vorrq_u8(d2, d3));
    return vmaxvq_u32(vreinterpretq_u32_u8(diff)) == 0;
#else
    return std::memcmp(a, b, 64) == 0;
#endif
}
} // namespace nce_detail

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
        const bool same = block_size == 64 ? nce_detail::EqualBlock64(current + block, snapshot + block)
                                           : (std::memcmp(current + block, snapshot + block, block_size) == 0);
        if (same) {
            finish(block);
        } else {
            for (u64 offset = block; offset < end;) {
                const u64 size = std::min(word, end - offset);
                bool same_word;
                if (size == 4) {
                    u32 cur_word;
                    u32 snap_word;
                    std::memcpy(&cur_word, current + offset, 4);
                    std::memcpy(&snap_word, snapshot + offset, 4);
                    same_word = (cur_word == snap_word);
                } else {
                    same_word = (std::memcmp(current + offset, snapshot + offset, size) == 0);
                }
                if (same_word) {
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
