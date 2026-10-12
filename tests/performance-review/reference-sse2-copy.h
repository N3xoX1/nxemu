// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace ReferenceV7 {

inline bool ReadbackMemoryEqual(const unsigned char* lhs, const unsigned char* rhs,
                                std::size_t size) {
#if defined(_M_X64) || defined(__x86_64__)
    while (size >= 64) {
        const auto a = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs)),
                                     _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs)));
        const auto b = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs + 16)),
                                     _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs + 16)));
        const auto c = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs + 32)),
                                     _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs + 32)));
        const auto d = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs + 48)),
                                     _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs + 48)));
        if (_mm_movemask_epi8(_mm_and_si128(_mm_and_si128(a, b), _mm_and_si128(c, d))) != 0xffff) {
            return false;
        }
        lhs += 64; rhs += 64; size -= 64;
    }
    while (size >= 16) {
        const auto equal = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs)),
                                         _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs)));
        if (_mm_movemask_epi8(equal) != 0xffff) {
            return false;
        }
        lhs += 16; rhs += 16; size -= 16;
    }
#endif
    return size == 0 || std::memcmp(lhs, rhs, size) == 0;
}

// Compare actual bytes, never a hash. The caller must exclude concurrent writers,
// just as for memcpy. Dense changes switch back to the original bulk copy.
inline std::size_t CopyChangedMemory(void* destination, const void* source, std::size_t size) {
    if (!size) {
        return 0;
    }
    const auto dst_address = reinterpret_cast<std::uintptr_t>(destination);
    const auto src_address = reinterpret_cast<std::uintptr_t>(source);
    const auto distance = dst_address >= src_address ? dst_address - src_address
                                                     : src_address - dst_address;
    if (distance < size) {
        std::memmove(destination, source, size);
        return size;
    }
    if (size < 1024 * 1024) {
        std::memcpy(destination, source, size);
        return size;
    }
    auto* dst = static_cast<unsigned char*>(destination);
    auto* src = static_cast<const unsigned char*>(source);
    std::size_t copied = 0;
    unsigned changed_run = 0;
    while (size) {
        const std::size_t chunk = std::min(size, std::size_t{64 * 1024});
        if (!ReadbackMemoryEqual(dst, src, chunk)) {
            if (++changed_run == 4) {
                std::memcpy(dst, src, size);
                return copied + size;
            }
            std::memcpy(dst, src, chunk);
            copied += chunk;
        } else {
            changed_run = 0;
        }
        dst += chunk;
        src += chunk;
        size -= chunk;
    }
    return copied;
}

} // namespace ReferenceV7
