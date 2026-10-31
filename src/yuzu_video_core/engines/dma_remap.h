// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <span>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSSE3__)
#include <tmmintrin.h>
#endif

#include "yuzu_video_core/engines/maxwell_dma.h"

namespace Tegra::Engines {

// With remapping enabled, DMA lengths, widths and X origins count elements of
// num_components * component_size bytes. Source and destination elements may
// differ in size; each destination component takes a source component, one of
// the two constants, or keeps its previous value (NO_WRITE).

inline std::array<MaxwellDMA::RemapConst::Swizzle, 4> RemapSwizzle(
    const MaxwellDMA::RemapConst& remap) {
    return {remap.dst_x, remap.dst_y, remap.dst_z, remap.dst_w};
}

inline u32 RemapComponentSize(const MaxwellDMA::RemapConst& remap) {
    return remap.component_size_minus_one + 1;
}

inline u32 RemapSrcBytesPerPixel(const MaxwellDMA::RemapConst& remap) {
    return (remap.num_src_components_minus_one + 1) * RemapComponentSize(remap);
}

inline u32 RemapDstBytesPerPixel(const MaxwellDMA::RemapConst& remap) {
    return (remap.num_dst_components_minus_one + 1) * RemapComponentSize(remap);
}

// Bytes per element when remapping writes every source component unchanged at its
// own position, which makes the transfer a plain byte copy. Zero otherwise.
inline u32 IdentityRemapBytesPerPixel(const MaxwellDMA::RemapConst& remap) {
    using Swizzle = MaxwellDMA::RemapConst::Swizzle;
    const u32 components = remap.num_dst_components_minus_one + 1;
    if (remap.num_src_components_minus_one + 1 != components) {
        return 0;
    }
    const auto swizzle = RemapSwizzle(remap);
    for (u32 i = 0; i < components; ++i) {
        if (swizzle[i] != static_cast<Swizzle>(static_cast<u32>(Swizzle::SRC_X) + i)) {
            return 0;
        }
    }
    return components * RemapComponentSize(remap);
}

inline bool RemapReadsSource(const MaxwellDMA::RemapConst& remap) {
    const auto swizzle = RemapSwizzle(remap);
    for (u32 i = 0; i <= remap.num_dst_components_minus_one; ++i) {
        if (swizzle[i] <= MaxwellDMA::RemapConst::Swizzle::SRC_W) {
            return true;
        }
    }
    return false;
}

inline bool RemapKeepsDestination(const MaxwellDMA::RemapConst& remap) {
    const auto swizzle = RemapSwizzle(remap);
    for (u32 i = 0; i <= remap.num_dst_components_minus_one; ++i) {
        // Values 6 and 7 both leave the component untouched.
        if (swizzle[i] >= MaxwellDMA::RemapConst::Swizzle::NO_WRITE) {
            return true;
        }
    }
    return false;
}

namespace Detail {

template <size_t Size, u32 dst_components>
void ApplyRemapSized(const MaxwellDMA::RemapConst& remap, std::span<const u8> src,
                     std::span<u8> dst, size_t count) {
    using Swizzle = MaxwellDMA::RemapConst::Swizzle;
    using Component = std::array<u8, Size>;
    const u32 src_components = remap.num_src_components_minus_one + 1;
    const auto swizzle = RemapSwizzle(remap);
    // Per destination component: source index, or -1 for a constant, -2 to keep.
    std::array<int, 4> source{};
    std::array<Component, 4> constant{};
    for (u32 c = 0; c < dst_components; ++c) {
        const Swizzle sw = swizzle[c];
        if (sw <= Swizzle::SRC_W) {
            const u32 index = static_cast<u32>(sw);
            source[c] = index < src_components ? static_cast<int>(index) : -1;
        } else if (sw == Swizzle::CONST_A || sw == Swizzle::CONST_B) {
            const u32 value =
                sw == Swizzle::CONST_A ? remap.remap_consta_value : remap.remap_constb_value;
            std::memcpy(constant[c].data(), &value, Size);
            source[c] = -1;
        } else {
            source[c] = -2;
        }
    }
    const u8* in = src.data();
    u8* out = dst.data();
    const size_t src_stride = size_t{src_components} * Size;
    for (size_t element = 0; element < count; ++element, in += src_stride) {
        for (u32 c = 0; c < dst_components; ++c, out += Size) {
            const int index = source[c];
            if (index >= 0) {
                std::memcpy(out, in + index * Size, Size);
            } else if (index == -1) {
                std::memcpy(out, constant[c].data(), Size);
            }
        }
    }
}

template <size_t Size>
void ApplyRemapSized(const MaxwellDMA::RemapConst& remap, std::span<const u8> src,
                     std::span<u8> dst, size_t count) {
    switch (remap.num_dst_components_minus_one) {
    case 0:
        return ApplyRemapSized<Size, 1>(remap, src, dst, count);
    case 1:
        return ApplyRemapSized<Size, 2>(remap, src, dst, count);
    case 2:
        return ApplyRemapSized<Size, 3>(remap, src, dst, count);
    default:
        return ApplyRemapSized<Size, 4>(remap, src, dst, count);
    }
}

} // namespace Detail

// When destination elements tile 16 bytes and as many source elements fit in 16
// bytes, the remap is a byte table lookup per group of elements:
// out = keep ? old : (source byte or constant).
struct RemapShuffle {
    u32 src_bytes_per_pixel;
    u32 dst_bytes_per_pixel;
    u32 src_group_bytes;         // source bytes consumed per 16 destination bytes
    std::array<u8, 16> index;    // source byte in the group, 0xFF for none
    std::array<u8, 16> constant; // zero where a source byte or kept byte goes
    std::array<u8, 16> keep;     // 0xFF where the destination byte is kept
    bool keeps;
};

inline std::optional<RemapShuffle> MakeRemapShuffle(const MaxwellDMA::RemapConst& remap) {
    using Swizzle = MaxwellDMA::RemapConst::Swizzle;
    const u32 size = RemapComponentSize(remap);
    const u32 src_components = remap.num_src_components_minus_one + 1;
    const u32 src_bpp = RemapSrcBytesPerPixel(remap);
    const u32 dst_bpp = RemapDstBytesPerPixel(remap);
    if (16 % dst_bpp != 0 || 16 / dst_bpp * src_bpp > 16) {
        return std::nullopt;
    }
    RemapShuffle shuffle{.src_bytes_per_pixel = src_bpp,
                         .dst_bytes_per_pixel = dst_bpp,
                         .src_group_bytes = 16 / dst_bpp * src_bpp,
                         .index{},
                         .constant{},
                         .keep{},
                         .keeps = false};
    const auto swizzle = RemapSwizzle(remap);
    for (u32 byte = 0; byte < 16; ++byte) {
        const u32 element = byte / dst_bpp;
        const u32 component = byte % dst_bpp / size;
        const u32 offset = byte % size;
        const Swizzle sw = swizzle[component];
        shuffle.index[byte] = 0xFF;
        if (sw <= Swizzle::SRC_W) {
            const u32 source = static_cast<u32>(sw);
            if (source < src_components) {
                shuffle.index[byte] = static_cast<u8>(element * src_bpp + source * size + offset);
            }
        } else if (sw == Swizzle::CONST_A || sw == Swizzle::CONST_B) {
            const u32 value =
                sw == Swizzle::CONST_A ? remap.remap_consta_value : remap.remap_constb_value;
            shuffle.constant[byte] = static_cast<u8>(value >> (offset * 8));
        } else {
            shuffle.keep[byte] = 0xFF;
            shuffle.keeps = true;
        }
    }
    return shuffle;
}

// Remaps `count` elements. `src` and `dst` may be equal when the element sizes are,
// but must not otherwise overlap.
inline void ApplyRemapShuffle(const RemapShuffle& shuffle, const u8* src, u8* dst,
                              size_t count) {
    const size_t dst_bytes = count * shuffle.dst_bytes_per_pixel;
    const size_t src_bytes = count * shuffle.src_bytes_per_pixel;
    const size_t step = shuffle.src_group_bytes;
    size_t in = 0;
    size_t out = 0;
#if defined(__ARM_NEON) || defined(__aarch64__)
    const uint8x16_t index = vld1q_u8(shuffle.index.data());
    const uint8x16_t constant = vld1q_u8(shuffle.constant.data());
    const uint8x16_t keep = vld1q_u8(shuffle.keep.data());
    // A group reads 16 source bytes even when it consumes fewer.
    for (; out + 16 <= dst_bytes && in + 16 <= src_bytes; out += 16, in += step) {
        uint8x16_t value = vorrq_u8(vqtbl1q_u8(vld1q_u8(src + in), index), constant);
        if (shuffle.keeps) {
            value = vbslq_u8(keep, vld1q_u8(dst + out), value);
        }
        vst1q_u8(dst + out, value);
    }
#elif defined(__SSSE3__)
    const __m128i index = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shuffle.index.data()));
    const __m128i constant =
        _mm_loadu_si128(reinterpret_cast<const __m128i*>(shuffle.constant.data()));
    const __m128i keep = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shuffle.keep.data()));
    for (; out + 16 <= dst_bytes && in + 16 <= src_bytes; out += 16, in += step) {
        const __m128i source = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + in));
        __m128i value = _mm_or_si128(_mm_shuffle_epi8(source, index), constant);
        if (shuffle.keeps) {
            const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + out));
            value = _mm_or_si128(_mm_and_si128(keep, old), _mm_andnot_si128(keep, value));
        }
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + out), value);
    }
#endif
    // Remaining groups, or all of them without SIMD. Elements never straddle a group.
    for (; out < dst_bytes; out += 16, in += step) {
        std::array<u8, 16> group{};
        std::memcpy(group.data(), src + in, std::min<size_t>(16, src_bytes - in));
        const size_t chunk = std::min<size_t>(16, dst_bytes - out);
        for (size_t byte = 0; byte < chunk; ++byte) {
            if (!shuffle.keep[byte]) {
                const u8 source = shuffle.index[byte];
                dst[out + byte] = source < 16 ? group[source] : shuffle.constant[byte];
            }
        }
    }
}

// Builds `count` destination elements from packed source elements. `dst` must hold
// the previous destination contents when RemapKeepsDestination(). Constants use
// their low component_size bytes. A source component beyond num_src_components
// reads as zero.
inline void ApplyRemap(const MaxwellDMA::RemapConst& remap, std::span<const u8> src,
                       std::span<u8> dst, size_t count) {
    if (const auto shuffle = MakeRemapShuffle(remap)) {
        return ApplyRemapShuffle(*shuffle, src.data(), dst.data(), count);
    }
    switch (RemapComponentSize(remap)) {
    case 1:
        return Detail::ApplyRemapSized<1>(remap, src, dst, count);
    case 2:
        return Detail::ApplyRemapSized<2>(remap, src, dst, count);
    case 3:
        return Detail::ApplyRemapSized<3>(remap, src, dst, count);
    default:
        return Detail::ApplyRemapSized<4>(remap, src, dst, count);
    }
}

} // namespace Tegra::Engines
