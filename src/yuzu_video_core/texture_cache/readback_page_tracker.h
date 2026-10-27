// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <map>
#include <vector>
#include "yuzu_common/common_types.h"

namespace VideoCommon {

// Tracks CPU observations separately from GPU ownership. A new observation must
// synchronously initialize its backing before relying on asynchronous publications.
class ReadbackPageTracker {
public:
    static constexpr u64 PAGE_SIZE = 4096;

    bool Observe(u64 address, u64 size) {
        bool known = true;
        ForPages(address, size, [&](u64 page) {
            if (!pages.contains(page)) {
                pages.emplace(page, Page{++serial, {{0, PAGE_SIZE, 0}}});
                known = false;
            }
        });
        return known;
    }

    u64 Serial() const { return serial; }

    void MarkSynchronized(u64 address, u64 size) {
        if (size == 0) return;
        const u64 stamp = ++serial;
        for (auto it = pages.lower_bound(address & ~(PAGE_SIZE - 1));
             it != pages.end() && it->first < address + size; ++it) {
            const u64 lo = std::max(address, it->first) - it->first;
            const u64 hi = std::min(address + size, it->first + PAGE_SIZE) - it->first;
            std::vector<Version> next;
            for (const auto& span : it->second.versions) {
                if (span.end <= lo || span.begin >= hi) { next.push_back(span); continue; }
                if (span.begin < lo) next.push_back({span.begin, lo, span.serial});
                next.push_back({std::max(span.begin, lo), std::min(span.end, hi), stamp});
                if (span.end > hi) next.push_back({hi, span.end, span.serial});
            }
            // Bound metadata to actual byte boundaries, not the number of readbacks.
            auto out = next.begin();
            for (auto in = next.begin(); in != next.end(); ++in) {
                if (out != next.begin() && (out - 1)->serial == in->serial) {
                    (out - 1)->end = in->end;
                } else *out++ = *in;
            }
            next.erase(out, next.end());
            it->second.versions = std::move(next);
        }
    }

    void Forget(u64 address, u64 size) {
        if (size == 0) return;
        auto first = pages.lower_bound(address & ~(PAGE_SIZE - 1));
        auto last = pages.lower_bound(address + size);
        pages.erase(first, last);
    }

    template <typename F>
    void ForEachRange(u64 address, u64 size, u64 snapshot_serial, F&& publish) const {
        if (size == 0) return;
        const u64 end = address + size;
        u64 begin{}, run_end{};
        const auto flush = [&] {
            if (begin < run_end) publish(begin - address, run_end - begin);
            begin = run_end = 0;
        };
        for (auto it = pages.lower_bound(address & ~(PAGE_SIZE - 1));
             it != pages.end() && it->first < end; ++it) {
            // An old copy cannot initialize a new observation or overwrite bytes
            // updated by a later synchronous copy, even through a partial-page alias.
            if (it->second.observed > snapshot_serial) { flush(); continue; }
            for (const auto& span : it->second.versions) {
                if (span.serial > snapshot_serial) { flush(); continue; }
                const u64 lo = std::max(address, it->first + span.begin);
                const u64 hi = std::min(end, it->first + span.end);
                if (lo >= hi) continue;
                if (begin < run_end && lo != run_end) flush();
                if (begin == run_end) begin = lo;
                run_end = hi;
            }
        }
        flush();
    }

private:
    template <typename F>
    static void ForPages(u64 address, u64 size, F&& f) {
        if (size == 0) return;
        const u64 end = address + size;
        for (u64 page = address & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) f(page);
    }
    struct Version { u64 begin; u64 end; u64 serial; };
    struct Page { u64 observed; std::vector<Version> versions; };
    std::map<u64, Page> pages;
    u64 serial{};
};

} // namespace VideoCommon
