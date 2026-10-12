// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>

namespace VideoCommon {

// Writers must retain the buffer-cache mutex. A positive page is conservative;
// only a negative allows the caller to omit that mutex. Publish before adding
// GPU-dirty bytes. Refresh only after RAM copies and metadata retirement finish,
// enumerating ALL remaining dirty bytes, including newer pending GPU epochs.
// This summary is never used to skip RAM reconciliation or a required GPU wait.
template<unsigned AddressBits = 34, unsigned PageBits = 16>
class GpuDirtySummary {
    static_assert(PageBits < AddressBits && AddressBits < 64);
    static constexpr std::uint64_t Limit = std::uint64_t{1} << AddressBits;
    static constexpr std::uint64_t WordBytes = std::uint64_t{64} << PageBits;
    static constexpr std::size_t WordCount = ((Limit >> PageBits) + 63) / 64;

public:
    void Mark(std::uint64_t address, std::uint64_t size) noexcept {
        if (!size) return;
        if (!Valid(address, size)) {
            unbounded.store(true, std::memory_order_release);
            return;
        }
        VisitWords(address, size, [&](std::size_t word, std::uint64_t mask) {
            // All writers, including Refresh, are serialized by the same cache
            // mutex. A word cannot clear between this check and dirty insertion.
            if ((pages[word].load(std::memory_order_relaxed) & mask) != mask)
                pages[word].fetch_or(mask, std::memory_order_release);
            return false;
        });
    }

    [[nodiscard]] bool MayHaveWrites(std::uint64_t address, std::uint64_t size) const noexcept {
        if (!size) return false;
        if (!Valid(address, size) || unbounded.load(std::memory_order_acquire)) return true;
        return VisitWords(address, size, [&](std::size_t word, std::uint64_t mask) {
            return (pages[word].load(std::memory_order_acquire) & mask) != 0;
        });
    }

    template<class EnumerateRemainingDirty>
    void Refresh(std::uint64_t address, std::uint64_t size, EnumerateRemainingDirty&& enumerate) {
        if (!size || !Valid(address, size)) return;
        const auto first = (address >> PageBits) >> 6;
        const auto last = ((address + size - 1) >> PageBits) >> 6;
        for (auto word = first; word <= last; ++word) {
            const auto old = pages[word].load(std::memory_order_relaxed);
            if (!old) continue;
            const auto base = word * WordBytes;
            const auto end = std::min(Limit, base + WordBytes);
            std::uint64_t mask{};
            // Rebuild the whole word. Clearing only the retired byte range
            // would lose overlapping/aliased dirty bytes and partial pages.
            enumerate(base, end - base, [&](std::uint64_t begin, std::uint64_t finish) {
                begin = std::max(begin, base);
                finish = std::min(finish, end);
                if (begin >= finish) return;
                const auto low = static_cast<unsigned>((begin - base) >> PageBits);
                const auto high = static_cast<unsigned>(((finish - 1 - base) >> PageBits) + 1);
                mask |= Mask(low, high);
            });
            if (old != mask) pages[word].store(mask, std::memory_order_release);
        }
    }

private:
    static bool Valid(std::uint64_t address, std::uint64_t size) {
        return address < Limit && size <= Limit - address;
    }
    static std::uint64_t Mask(unsigned low, unsigned high) {
        return (~std::uint64_t{0} << low) &
               (high == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << high) - 1);
    }
    template<class F>
    static bool VisitWords(std::uint64_t address, std::uint64_t size, F&& visit) {
        const auto first = address >> PageBits, last = (address + size - 1) >> PageBits;
        for (auto page = first; page <= last;) {
            const auto word = page >> 6;
            const auto low = static_cast<unsigned>(page & 63);
            const auto high = static_cast<unsigned>((last >> 6) == word ? (last & 63) + 1 : 64);
            if (visit(static_cast<std::size_t>(word), Mask(low, high))) return true;
            page = (word + 1) << 6;
        }
        return false;
    }
    std::array<std::atomic<std::uint64_t>, WordCount> pages{};
    std::atomic<bool> unbounded{};
};

inline std::unique_ptr<GpuDirtySummary<>> MakeGpuDirtySummary() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("NXEMU_CURRENT_GPU_DIRTY_FILTER");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    if (!enabled) return {};
    try { return std::make_unique<GpuDirtySummary<>>(); }
    catch (const std::bad_alloc&) { return {}; }
}
} // namespace VideoCommon
