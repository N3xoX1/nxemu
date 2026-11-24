// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

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
    /// Returns true when a page was not marked yet
    bool Mark(std::uint64_t address, std::uint64_t size) noexcept {
        if (!size) return false;
        if (address >= Limit || size > Limit-address) {
            return !unbounded.exchange(true,std::memory_order_acq_rel);
        }
        bool added = false;
        VisitWords(address,size,[&](size_t word,std::uint64_t mask) {
            // Published bits never clear, so avoid rewriting an unchanged word.
            if ((pages[word].load(std::memory_order_relaxed)&mask)==mask) return false;
            pages[word].fetch_or(mask,std::memory_order_release);
            added = true;
            return false;
        });
        return added;
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

// Clearable variant: the owner keeps it exact under its own mutex, readers may query it
// lock-free. Out-of-range addresses are always reported.
template<unsigned AddressBits = 34, unsigned PageBits = 12>
class AtomicPageBits {
    static constexpr std::uint64_t Limit = std::uint64_t{1} << AddressBits;
    static constexpr size_t WordCount = ((Limit >> PageBits) + 63) / 64;

public:
    static constexpr std::uint64_t PAGE_SIZE = std::uint64_t{1} << PageBits;

    void Set(std::uint64_t address, std::uint64_t size) noexcept {
        if (!size) return;
        if (address >= Limit || size > Limit - address) {
            unbounded.store(true, std::memory_order_release);
            return;
        }
        Visit(address, size, [&](size_t word, std::uint64_t mask) {
            if ((pages[word].load(std::memory_order_relaxed) & mask) != mask)
                pages[word].fetch_or(mask, std::memory_order_release);
            return false;
        });
    }

    void Clear(std::uint64_t address, std::uint64_t size) noexcept {
        if (!size || address >= Limit || size > Limit - address) return;
        Visit(address, size, [&](size_t word, std::uint64_t mask) {
            if (pages[word].load(std::memory_order_relaxed) & mask)
                pages[word].fetch_and(~mask, std::memory_order_release);
            return false;
        });
    }

    [[nodiscard]] bool Any(std::uint64_t address, std::uint64_t size) const noexcept {
        if (!size) return false;
        if (address >= Limit || size > Limit - address || unbounded.load(std::memory_order_acquire))
            return true;
        return Visit(address, size, [&](size_t word, std::uint64_t mask) {
            return (pages[word].load(std::memory_order_acquire) & mask) != 0;
        });
    }

private:
    template<class F> static bool Visit(std::uint64_t address, std::uint64_t size, F&& visit) {
        const auto first = address >> PageBits, last = (address + size - 1) >> PageBits;
        for (auto page = first; page <= last;) {
            const auto word = page >> 6;
            const unsigned begin = static_cast<unsigned>(page & 63);
            const unsigned end = static_cast<unsigned>((last >> 6) == word ? (last & 63) + 1 : 64);
            const std::uint64_t mask = (~std::uint64_t{0} << begin) &
                (end == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << end) - 1);
            if (visit(static_cast<size_t>(word), mask)) return true;
            page = (word + 1) << 6;
        }
        return false;
    }
    std::array<std::atomic<std::uint64_t>, WordCount> pages{};
    std::atomic<bool> unbounded{};
};
// Pages compute dispatches wrote, marked by the GPU thread before a dispatch runs and probed
// lock-free by the decode thread for the pushbuffer entries it fetches meanwhile
class ComputeWriteHistory {
public:
    void Mark(std::uint64_t address, std::uint64_t size) noexcept {
        // Dispatches write the same buffers and images again: the pages they marked stay marked
        const std::uint64_t end = address + size;
        for (const auto& [begin, marked_end] : marked) {
            if (begin <= address && end <= marked_end) return;
        }
        marked[next++ % marked.size()] = {address, end};
        if (pages->Mark(address, size)) {
            epoch.fetch_add(1, std::memory_order_release);
        }
    }

    [[nodiscard]] bool MayHaveWrites(std::uint64_t address, std::uint64_t size) const noexcept {
        return pages->MayHaveWrites(address, size);
    }

    /// Changes when a page no dispatch wrote before is marked
    [[nodiscard]] std::uint64_t Epoch() const noexcept {
        return epoch.load(std::memory_order_acquire);
    }

private:
    std::unique_ptr<GpuWriteHistory<34, 12>> pages = std::make_unique<GpuWriteHistory<34, 12>>();
    std::atomic<std::uint64_t> epoch{};
    std::array<std::pair<std::uint64_t, std::uint64_t>, 16> marked{};
    size_t next{};
};
} // namespace VideoCommon
