// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace VideoCommon {
// Only proves that a region has NEVER been GPU-dirty. No clearing means CPU
// cancellation, pending downloads, canonical deletion and address reuse cannot
// create false negatives. Publish before any dirty range or GPU write command.
// Positive results must still consult the cache under its original mutex.
template<unsigned AddressBits = 34, unsigned PageBits = 16>
class GpuWriteHistory {
    static_assert(PageBits < AddressBits && AddressBits < 64);
    static constexpr std::uint64_t Limit = std::uint64_t{1} << AddressBits;
    static constexpr size_t WordCount = ((Limit >> PageBits) + 63) / 64;

public:
    void Mark(std::uint64_t address, std::uint64_t size) noexcept {
        if (!size) return;
        if (address >= Limit || size > Limit-address) {
            unbounded.store(true,std::memory_order_release);
            return;
        }
        VisitWords(address,size,[&](size_t word,std::uint64_t mask) {
            // Published bits never clear, so avoid rewriting an unchanged word.
            if ((pages[word].load(std::memory_order_relaxed)&mask)==mask) return false;
            pages[word].fetch_or(mask,std::memory_order_release);
            return false;
        });
    }

    [[nodiscard]] bool MayHaveWrites(std::uint64_t address,std::uint64_t size) const noexcept {
        if (!size) return false;
        if (address >= Limit || size > Limit-address || unbounded.load(std::memory_order_acquire)) return true;
        return VisitWords(address,size,[&](size_t word,std::uint64_t mask) {
            return (pages[word].load(std::memory_order_acquire)&mask)!=0;
        });
    }

private:
    template<class F> static bool VisitWords(std::uint64_t address,std::uint64_t size,F&& visit) {
        const auto first=address>>PageBits, last=(address+size-1)>>PageBits;
        for (auto page=first;page<=last;) {
            const auto word=page>>6;
            const unsigned begin=static_cast<unsigned>(page&63);
            const unsigned end=static_cast<unsigned>((last>>6)==word ? (last&63)+1 : 64);
            const std::uint64_t mask=(~std::uint64_t{0}<<begin)&
                (end==64 ? ~std::uint64_t{0} : (std::uint64_t{1}<<end)-1);
            if (visit(static_cast<size_t>(word),mask)) return true;
            page=(word+1)<<6;
        }
        return false;
    }
    std::array<std::atomic<std::uint64_t>,WordCount> pages{};
    std::atomic<bool> unbounded{};
};
} // namespace VideoCommon
