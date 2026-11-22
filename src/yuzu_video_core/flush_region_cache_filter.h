// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include "yuzu_video_core/cache_types.h"

namespace VideoCommon {

// Conservative presence filter, not a dirty-memory tracker. Bits are never cleared:
// deletion, remapping and address reuse can only produce extra cache consultations.
// Register each resource before publishing it in the cache's lookup tables.
template <unsigned AddressBits = 34, unsigned PageBits = 16>
class FlushRegionCacheFilter {
    static_assert(PageBits < AddressBits && AddressBits < 64);
    static constexpr u64 AddressLimit = u64{1} << AddressBits;
    static constexpr size_t WordCount = ((AddressLimit >> PageBits) + 63) / 64;
    static constexpr std::array Types{CacheType::TextureCache, CacheType::QueryCache,
                                      CacheType::BufferCache};

public:
    void Register(DAddr addr, u64 size, CacheType types) noexcept {
        if (size == 0) {
            return;
        }
        if (addr >= AddressLimit || size > AddressLimit - addr) {
            // An invalid/unsupported registration must not leave a false negative.
            unbounded.fetch_or(static_cast<u32>(types), std::memory_order_release);
            return;
        }
        const u64 first = addr >> PageBits;
        const u64 last = (addr + size - 1) >> PageBits;
        for (size_t cache = 0; cache < Types.size(); ++cache) {
            if (False(types & Types[cache])) {
                continue;
            }
            for (u64 page = first; page <= last;) {
                const unsigned begin = static_cast<unsigned>(page & 63);
                const u64 word = page >> 6;
                const unsigned end = static_cast<unsigned>(
                    (last >> 6) == word ? (last & 63) + 1 : 64);
                const u64 mask = (~u64{0} << begin) &
                                 (end == 64 ? ~u64{0} : (u64{1} << end) - 1);
                pages[cache][word].fetch_or(mask, std::memory_order_release);
                page = (word + 1) << 6;
            }
        }
    }

    [[nodiscard]] CacheType Filter(DAddr addr, u64 size, CacheType requested) const noexcept {
        if (size == 0) {
            return CacheType::None;
        }
        if (addr >= AddressLimit || size > AddressLimit - addr) {
            return requested;
        }
        const auto fallback = static_cast<CacheType>(unbounded.load(std::memory_order_acquire));
        const u64 first = addr >> PageBits;
        const u64 last = (addr + size - 1) >> PageBits;
        for (size_t cache = 0; cache < Types.size(); ++cache) {
            if (False(requested & Types[cache]) || True(fallback & Types[cache])) {
                continue;
            }
            bool present = false;
            for (u64 page = first; page <= last; ++page) {
                if ((pages[cache][page >> 6].load(std::memory_order_acquire) &
                     (u64{1} << (page & 63))) != 0) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                requested &= ~Types[cache];
            }
        }
        return requested;
    }

private:
    std::array<std::array<std::atomic<u64>, WordCount>, Types.size()> pages{};
    std::atomic<u32> unbounded{};
};

} // namespace VideoCommon
