// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "yuzu_common/alignment.h"
#include "yuzu_common/common_types.h"
#include "yuzu_common/range_sets.h"

namespace VideoCommon {

/// The bytes the GPU wrote that guest memory doesn't hold yet. Calls on_pages(addr, size, 1)
/// for pages that gain their first range and on_pages(addr, size, -1) for pages that lose
/// their last one.
template <typename AddressType, u64 PAGE_SIZE>
class GpuModifiedRanges {
public:
    using OnPages = std::function<void(AddressType, u64, s32)>;

    void SetOnPages(OnPages on_pages_) {
        on_pages = std::move(on_pages_);
    }

    void Add(AddressType addr, u64 size) {
        ranges.Add(addr, size);
        const AddressType pages_start = Common::AlignDown(addr, PAGE_SIZE);
        const AddressType pages_end = Common::AlignUp(addr + size, PAGE_SIZE);
        ForEachGap(pages, pages_start, pages_end, [&](AddressType start, AddressType end) {
            Notify(start, end, 1);
        });
        pages.Add(pages_start, pages_end - pages_start);
    }

    void Subtract(AddressType addr, u64 size) {
        ranges.Subtract(addr, size);
        // Only the first and the last page can still hold other ranges
        AddressType pages_start = Common::AlignDown(addr, PAGE_SIZE);
        AddressType pages_end = Common::AlignUp(addr + size, PAGE_SIZE);
        if (pages_start != addr && Intersects(pages_start, PAGE_SIZE)) {
            pages_start += PAGE_SIZE;
        }
        if (pages_end != addr + size && pages_start < pages_end &&
            Intersects(pages_end - PAGE_SIZE, PAGE_SIZE)) {
            pages_end -= PAGE_SIZE;
        }
        if (pages_start >= pages_end) {
            return;
        }
        pages.ForEachInRange(pages_start, pages_end - pages_start,
                             [&](AddressType start, AddressType end) { Notify(start, end, -1); });
        pages.Subtract(pages_start, pages_end - pages_start);
    }

    [[nodiscard]] bool Empty() const {
        return ranges.Empty();
    }

    template <typename Func>
    void ForEachInRange(AddressType addr, u64 size, Func&& func) const {
        ranges.ForEachInRange(addr, size, std::forward<Func>(func));
    }

    /// Calls func(start, end) for each part of the range that no GPU written range covers
    template <typename Func>
    void ForEachNotInRange(AddressType addr, u64 size, Func&& func) const {
        ForEachGap(ranges, addr, addr + size, std::forward<Func>(func));
    }

private:
    template <typename Func>
    static void ForEachGap(const Common::RangeSet<AddressType>& set, AddressType start,
                           AddressType end, Func&& func) {
        // Collected first: func may change the set
        boost::container::small_vector<std::pair<AddressType, AddressType>, 4> gaps;
        AddressType gap_start = start;
        set.ForEachInRange(start, end - start, [&](AddressType range_start, AddressType range_end) {
            if (gap_start != range_start) {
                gaps.emplace_back(gap_start, range_start);
            }
            gap_start = range_end;
        });
        if (gap_start != end) {
            gaps.emplace_back(gap_start, end);
        }
        for (const auto& [gap_begin, gap_end] : gaps) {
            func(gap_begin, gap_end);
        }
    }

    [[nodiscard]] bool Intersects(AddressType addr, u64 size) const {
        bool found = false;
        ranges.ForEachInRange(addr, size, [&](AddressType, AddressType) { found = true; });
        return found;
    }

    void Notify(AddressType start, AddressType end, s32 delta) {
        if (on_pages) {
            on_pages(start, end - start, delta);
        }
    }

    Common::RangeSet<AddressType> ranges;
    Common::RangeSet<AddressType> pages;
    OnPages on_pages;
};

} // namespace VideoCommon
