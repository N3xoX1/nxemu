// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include "yuzu_common/common_types.h"

namespace VideoCommon {
struct ObservedReadbackRect {
    u32 x, y, width, height;
};

// Cover observed tiled bytes with rectangles in the linear GPU image. Keep the
// original full-image buffer stride so range publication can use the same offsets.
// nullopt selects the existing full copy for dense or overly fragmented coverage.
inline std::optional<std::vector<ObservedReadbackRect>> PlanObservedReadback(
    u32 row_bytes, u32 height, u32 block_height,
    std::span<const std::pair<u64, u64>> ranges, size_t max_rectangles = 64) {
    if (!row_bytes || row_bytes % 4 || !height || block_height > 5) return std::nullopt;
    const u64 width = row_bytes / 4;
    const u64 gobs_x = (u64{row_bytes} + 63) / 64;
    const u64 column_bytes = 512ULL << block_height;
    const u64 block_bytes = gobs_x * column_bytes;
    std::vector<u64> gobs;
    for (auto [offset, size] : ranges) {
        if (size > std::numeric_limits<u64>::max() - offset) return std::nullopt;
        if (!size) continue;
        const u64 first = offset / 512, last = (offset + size - 1) / 512;
        if (last - first >= 8192 || gobs.size() + last - first + 1 > 8192) return std::nullopt;
        for (u64 gob = first; gob <= last; ++gob) gobs.push_back(gob);
    }
    std::ranges::sort(gobs);
    gobs.erase(std::unique(gobs.begin(), gobs.end()), gobs.end());
    std::vector<ObservedReadbackRect> rows;
    for (u64 gob : gobs) {
        const u64 byte = gob * 512;
        const u64 x = ((byte % block_bytes) / column_bytes) * 16;
        const u64 y = (byte / block_bytes) * (8ULL << block_height) +
                      ((byte % column_bytes) / 512) * 8;
        if (x >= width || y >= height) continue;
        rows.push_back({u32(x), u32(y), u32(std::min<u64>(16, width - x)),
                       u32(std::min<u64>(8, height - y))});
    }
    std::ranges::sort(rows, [](const auto& a, const auto& b) {
        return std::pair{a.y, a.x} < std::pair{b.y, b.x};
    });
    auto out = rows.begin();
    for (auto it = rows.begin(); it != rows.end(); ++it) {
        if (out != rows.begin() && (out - 1)->y == it->y &&
            (out - 1)->height == it->height && (out - 1)->x + (out - 1)->width == it->x) {
            (out - 1)->width += it->width;
        } else *out++ = *it;
    }
    rows.erase(out, rows.end());
    std::vector<ObservedReadbackRect> result;
    std::map<std::pair<u32, u32>, size_t> columns;
    u64 bytes{};
    for (const auto& row : rows) {
        bytes += u64{row.width} * row.height * 4;
        const auto key = std::pair{row.x, row.width};
        auto found = columns.find(key);
        if (found != columns.end() && result[found->second].y + result[found->second].height == row.y) {
            result[found->second].height += row.height;
        } else {
            if (result.size() >= max_rectangles) return std::nullopt;
            columns[key] = result.size();
            result.push_back(row);
        }
    }
    if (bytes >= u64{row_bytes} * height * 3 / 4) return std::nullopt;
    return result;
}
} // namespace VideoCommon
