// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
#include <arm_neon.h>
#elif defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

namespace Common {

namespace ReadbackDetail {

using Comparer = bool (*)(const unsigned char*, const unsigned char*, std::size_t);

inline bool EqualPortable(const unsigned char* lhs, const unsigned char* rhs, std::size_t size) {
    return size == 0 || std::memcmp(lhs, rhs, size) == 0;
}

#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
inline bool EqualNeon(const unsigned char* lhs, const unsigned char* rhs, std::size_t size) {
    while (size >= 64) {
        const auto a = vceqq_u8(vld1q_u8(lhs), vld1q_u8(rhs));
        const auto b = vceqq_u8(vld1q_u8(lhs + 16), vld1q_u8(rhs + 16));
        const auto c = vceqq_u8(vld1q_u8(lhs + 32), vld1q_u8(rhs + 32));
        const auto d = vceqq_u8(vld1q_u8(lhs + 48), vld1q_u8(rhs + 48));
        if (vminvq_u8(vandq_u8(vandq_u8(a, b), vandq_u8(c, d))) != 0xff) {
            return false;
        }
        lhs += 64;
        rhs += 64;
        size -= 64;
    }
    while (size >= 16) {
        if (vminvq_u8(vceqq_u8(vld1q_u8(lhs), vld1q_u8(rhs))) != 0xff) {
            return false;
        }
        lhs += 16;
        rhs += 16;
        size -= 16;
    }
    return EqualPortable(lhs, rhs, size);
}
#elif defined(_M_X64) || defined(__x86_64__)
inline bool EqualSse2(const unsigned char* lhs, const unsigned char* rhs, std::size_t size) {
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
        lhs += 64;
        rhs += 64;
        size -= 64;
    }
    while (size >= 16) {
        const auto equal = _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(lhs)),
                                         _mm_loadu_si128(reinterpret_cast<const __m128i*>(rhs)));
        if (_mm_movemask_epi8(equal) != 0xffff) {
            return false;
        }
        lhs += 16;
        rhs += 16;
        size -= 16;
    }
    return EqualPortable(lhs, rhs, size);
}

// Keep AVX2 instructions out of the baseline caller. GCC/Clang compile this
// function for AVX2 only; MSVC explicit intrinsics need no global /arch setting.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((target("avx2"), noinline))
#endif
inline bool EqualAvx2(const unsigned char* lhs, const unsigned char* rhs, std::size_t size) {
    while (size >= 128) {
        const auto a = _mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs)));
        const auto b = _mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 32)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 32)));
        const auto c = _mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 64)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 64)));
        const auto d = _mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(lhs + 96)),
                                        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rhs + 96)));
        if (_mm256_movemask_epi8(_mm256_and_si256(_mm256_and_si256(a, b), _mm256_and_si256(c, d))) != -1) {
            return false;
        }
        lhs += 128;
        rhs += 128;
        size -= 128;
    }
    return EqualSse2(lhs, rhs, size);
}

// Feature selection is cached once, and includes the OS's YMM context support.
inline bool HasAvx2() {
#if defined(_MSC_VER)
    int registers[4];
    __cpuidex(registers, 0, 0);
    if (registers[0] < 7) {
        return false;
    }
    __cpuidex(registers, 1, 0);
    constexpr unsigned required = (1U << 26) | (1U << 27) | (1U << 28);
    if ((static_cast<unsigned>(registers[2]) & required) != required || (_xgetbv(0) & 6) != 6) {
        return false;
    }
    __cpuidex(registers, 7, 0);
    return (registers[1] & (1 << 5)) != 0;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#endif
}
#endif

inline Comparer SelectComparer() {
#if defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
    // Advanced SIMD is part of the AArch64 baseline, including Apple Silicon
    // and Android arm64. No CPU-vendor or platform-specific tuning is needed.
    return EqualNeon;
#elif defined(_M_X64) || defined(__x86_64__)
    return HasAvx2() ? EqualAvx2 : EqualSse2;
#else
    return EqualPortable;
#endif
}
} // namespace ReadbackDetail

inline bool ReadbackMemoryEqual(const unsigned char* lhs, const unsigned char* rhs,
                                std::size_t size) {
    static const auto comparer = ReadbackDetail::SelectComparer();
    return comparer(lhs, rhs, size);
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

} // namespace Common
