#pragma once

#include <algorithm>
#include <cstddef>
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

namespace nce_detail {
inline bool WordEquals(const u8 * a, const u8 * b) noexcept {
    u32 wa;
    u32 wb;
    std::memcpy(&wa, a, 4);
    std::memcpy(&wb, b, 4);
    return wa == wb;
}
} // namespace nce_detail

// Merges a GPU readback chunk [chunk_off, chunk_off + chunk_size) inside a page into
// page_dst and its CPU-fault snapshot page_ref using the same 4-byte aligned word
// granularity as ForEachNceWrittenRange. Any aligned 4-byte word where at least one
// byte differs from the snapshot is treated as CPU-owned and kept intact in both
// page_dst and page_ref so the subsequent flush reports the complete CPU word.
// Caller guarantees chunk_off + chunk_size <= page_size and page_size is a multiple of 4.
inline void MergeNceReadbackWords(u8 * page_ref, u8 * page_dst, u64 chunk_off, const u8 * src,
                                  u64 chunk_size) noexcept
{
    if (chunk_size == 0)
    {
        return;
    }
    const u64 chunk_end = chunk_off + chunk_size;
    const u64 span_begin = chunk_off & ~u64{3};
    const u64 span_end = (chunk_end + 3) & ~u64{3};

    // Fast path: no overlapping 4-byte word was modified by the CPU -> bulk copy.
    if (std::memcmp(page_dst + span_begin, page_ref + span_begin, span_end - span_begin) == 0)
    {
        std::memcpy(page_dst + chunk_off, src, chunk_size);
        std::memcpy(page_ref + chunk_off, src, chunk_size);
        return;
    }

    u64 pos = chunk_off;
    if ((pos & 3) != 0)
    {
        const u64 word_off = pos & ~u64{3};
        const u64 partial_end = std::min(word_off + 4, chunk_end);
        const u64 partial_len = partial_end - pos;
        if (nce_detail::WordEquals(page_dst + word_off, page_ref + word_off))
        {
            std::memcpy(page_dst + pos, src + (pos - chunk_off), partial_len);
            std::memcpy(page_ref + pos, src + (pos - chunk_off), partial_len);
        }
        pos = partial_end;
    }

    const u64 aligned_end = chunk_end & ~u64{3};
#if defined(__aarch64__)
    for (; pos + 16 <= aligned_end; pos += 16)
    {
        const uint32x4_t d = vreinterpretq_u32_u8(vld1q_u8(page_dst + pos));
        const uint32x4_t r = vreinterpretq_u32_u8(vld1q_u8(page_ref + pos));
        const uint32x4_t s = vreinterpretq_u32_u8(vld1q_u8(src + (pos - chunk_off)));
        const uint32x4_t eq = vceqq_u32(d, r);
        vst1q_u8(page_dst + pos, vreinterpretq_u8_u32(vbslq_u32(eq, s, d)));
        vst1q_u8(page_ref + pos, vreinterpretq_u8_u32(vbslq_u32(eq, s, r)));
    }
#endif
    for (; pos + 4 <= aligned_end; pos += 4)
    {
        if (nce_detail::WordEquals(page_dst + pos, page_ref + pos))
        {
            std::memcpy(page_dst + pos, src + (pos - chunk_off), 4);
            std::memcpy(page_ref + pos, src + (pos - chunk_off), 4);
        }
    }

    if (pos < chunk_end)
    {
        const u64 tail_len = chunk_end - pos;
        if (nce_detail::WordEquals(page_dst + pos, page_ref + pos))
        {
            std::memcpy(page_dst + pos, src + (pos - chunk_off), tail_len);
            std::memcpy(page_ref + pos, src + (pos - chunk_off), tail_len);
        }
    }
}

// Multi-snapshot merge (page present in both nce_written and nce_flushing, or mapped via
// multiple CPU VA aliases): an aligned 4-byte word is untouched by the CPU iff all 4 bytes
// equal EVERY snapshot.
inline void MergeNceReadbackWordsMulti(u8 * const * page_refs, size_t ref_count, u8 * page_dst,
                                       u64 chunk_off, const u8 * src, u64 chunk_size) noexcept
{
    if (chunk_size == 0)
    {
        return;
    }
    if (ref_count == 1)
    {
        MergeNceReadbackWords(page_refs[0], page_dst, chunk_off, src, chunk_size);
        return;
    }
    const u64 chunk_end = chunk_off + chunk_size;
    const u64 span_begin = chunk_off & ~u64{3};
    const u64 span_end = (chunk_end + 3) & ~u64{3};

    bool all_equal = true;
    for (size_t k = 0; k < ref_count; ++k)
    {
        if (std::memcmp(page_dst + span_begin, page_refs[k] + span_begin, span_end - span_begin) != 0)
        {
            all_equal = false;
            break;
        }
    }
    if (all_equal)
    {
        std::memcpy(page_dst + chunk_off, src, chunk_size);
        for (size_t k = 0; k < ref_count; ++k)
        {
            std::memcpy(page_refs[k] + chunk_off, src, chunk_size);
        }
        return;
    }

    const auto word_untouched = [&](u64 word_off) noexcept {
        for (size_t k = 0; k < ref_count; ++k)
        {
            if (!nce_detail::WordEquals(page_dst + word_off, page_refs[k] + word_off))
            {
                return false;
            }
        }
        return true;
    };

    u64 pos = chunk_off;
    if ((pos & 3) != 0)
    {
        const u64 word_off = pos & ~u64{3};
        const u64 partial_end = std::min(word_off + 4, chunk_end);
        const u64 partial_len = partial_end - pos;
        if (word_untouched(word_off))
        {
            std::memcpy(page_dst + pos, src + (pos - chunk_off), partial_len);
            for (size_t k = 0; k < ref_count; ++k)
            {
                std::memcpy(page_refs[k] + pos, src + (pos - chunk_off), partial_len);
            }
        }
        pos = partial_end;
    }

    const u64 aligned_end = chunk_end & ~u64{3};
#if defined(__aarch64__)
    for (; pos + 16 <= aligned_end; pos += 16)
    {
        const uint32x4_t d = vreinterpretq_u32_u8(vld1q_u8(page_dst + pos));
        const uint32x4_t s = vreinterpretq_u32_u8(vld1q_u8(src + (pos - chunk_off)));
        uint32x4_t eq = vdupq_n_u32(0xFFFFFFFFu);
        for (size_t k = 0; k < ref_count; ++k)
        {
            const uint32x4_t rk = vreinterpretq_u32_u8(vld1q_u8(page_refs[k] + pos));
            eq = vandq_u32(eq, vceqq_u32(d, rk));
        }
        vst1q_u8(page_dst + pos, vreinterpretq_u8_u32(vbslq_u32(eq, s, d)));
        for (size_t k = 0; k < ref_count; ++k)
        {
            const uint32x4_t rk = vreinterpretq_u32_u8(vld1q_u8(page_refs[k] + pos));
            vst1q_u8(page_refs[k] + pos, vreinterpretq_u8_u32(vbslq_u32(eq, s, rk)));
        }
    }
#endif
    for (; pos + 4 <= aligned_end; pos += 4)
    {
        if (word_untouched(pos))
        {
            std::memcpy(page_dst + pos, src + (pos - chunk_off), 4);
            for (size_t k = 0; k < ref_count; ++k)
            {
                std::memcpy(page_refs[k] + pos, src + (pos - chunk_off), 4);
            }
        }
    }

    if (pos < chunk_end)
    {
        const u64 tail_len = chunk_end - pos;
        if (word_untouched(pos))
        {
            std::memcpy(page_dst + pos, src + (pos - chunk_off), tail_len);
            for (size_t k = 0; k < ref_count; ++k)
            {
                std::memcpy(page_refs[k] + pos, src + (pos - chunk_off), tail_len);
            }
        }
    }
}

} // namespace Core
