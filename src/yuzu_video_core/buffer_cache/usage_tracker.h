// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "yuzu_common/alignment.h"
#include "yuzu_common/common_types.h"

namespace VideoCommon {

class UsageTracker {
    static constexpr size_t BYTES_PER_BIT_SHIFT = 6;
    static constexpr size_t PAGE_SHIFT = 6 + BYTES_PER_BIT_SHIFT;
    static constexpr size_t PAGE_BYTES = 1 << PAGE_SHIFT;

public:
    explicit UsageTracker(size_t size) {
        const size_t num_pages = (size >> PAGE_SHIFT) + 1;
        pages.resize(num_pages, 0ULL);
    }

    void Reset() noexcept {
        std::ranges::fill(pages, 0ULL);
    }

    void Track(u64 offset, u64 size) noexcept {
        if (size == 0) {
            return;
        }
        const size_t page = offset >> PAGE_SHIFT;
        const size_t page_end = (offset + size) >> PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return;
        }
        TrackPage(page, offset, size);
        if (page == page_end) {
            return;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            pages[i] = ~u64{0};
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, PAGE_BYTES);
        TrackPage(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

    [[nodiscard]] bool IsUsed(u64 offset, u64 size) const noexcept {
        if (size == 0) {
            return false;
        }
        const size_t page = offset >> PAGE_SHIFT;
        const size_t page_end = (offset + size) >> PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return false;
        }
        if (IsPageUsed(page, offset, size)) {
            return true;
        }
        if (page == page_end) {
            return false;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            if (pages[i] != 0) {
                return true;
            }
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, PAGE_BYTES);
        return IsPageUsed(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

private:
    static u64 PageMask(u64 offset, u64 size) noexcept {
        if (size == 0) {
            return 0;
        }
        const size_t offset_in_page = offset % PAGE_BYTES;
        const size_t first_bit = offset_in_page >> BYTES_PER_BIT_SHIFT;
        const size_t end_offset = std::min<u64>(offset_in_page + size, PAGE_BYTES);
        const size_t end_bit =
            Common::AlignUp(end_offset, size_t{1} << BYTES_PER_BIT_SHIFT) >> BYTES_PER_BIT_SHIFT;
        // Every touched 64-byte block is used, including unaligned and sub-block writes.
        return (~u64{0} << first_bit) & (~u64{0} >> (64 - end_bit));
    }

    void TrackPage(u64 page, u64 offset, u64 size) noexcept {
        pages[page] |= PageMask(offset, size);
    }

    bool IsPageUsed(u64 page, u64 offset, u64 size) const noexcept {
        return (pages[page] & PageMask(offset, size)) != 0;
    }

private:
    std::vector<u64> pages;
};

} // namespace VideoCommon
