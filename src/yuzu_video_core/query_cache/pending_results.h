// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>

#include "yuzu_common/common_types.h"

namespace VideoCommon {

class PendingResults {
public:
    void Add(VAddr addr, u64 size, u32 location, bool skips_write_when_replaced) {
        results.emplace(addr, Result{location, size, skips_write_when_replaced});
    }

    void Completed(VAddr addr, u32 location) {
        if (const auto it = Find(addr, location); it != results.end()) {
            results.erase(it);
        }
    }

    void Replaced(VAddr addr, u32 location) {
        // An ordinary report still writes when its fence signals, possibly past a shorter one
        if (const auto it = Find(addr, location);
            it != results.end() && it->second.skips_write_when_replaced) {
            results.erase(it);
        }
    }

    [[nodiscard]] bool Any(VAddr addr, u64 size) const {
        const VAddr begin = addr >= TIMESTAMPED_RESULT_SIZE - 1
                                ? addr - (TIMESTAMPED_RESULT_SIZE - 1)
                                : 0;
        for (auto it = results.lower_bound(begin); it != results.end() && it->first < addr + size;
             ++it) {
            if (it->first + it->second.size > addr) {
                return true;
            }
        }
        return false;
    }

private:
    static constexpr u64 TIMESTAMPED_RESULT_SIZE = 16;

    struct Result {
        u32 location;
        u64 size;
        bool skips_write_when_replaced;
    };
    using Map = std::multimap<VAddr, Result>;

    Map::iterator Find(VAddr addr, u32 location) {
        const auto [begin, end] = results.equal_range(addr);
        for (auto it = begin; it != end; ++it) {
            if (it->second.location == location) {
                return it;
            }
        }
        return results.end();
    }

    Map results;
};

} // namespace VideoCommon
