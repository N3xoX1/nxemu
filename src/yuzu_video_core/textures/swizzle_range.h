// SPDX-FileCopyrightText: Copyright 2026 NXEmu Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <span>
#include "yuzu_common/common_types.h"

namespace Tegra::Texture {

// Publish only a byte range of a single-layer, single-mip 2D block-linear image.
// Output aliases that range, not the complete tiled allocation. Padding is preserved.
inline void Swizzle2DRange(std::span<u8> output, std::span<const u8> input,
                           u32 row_bytes, u32 height, u32 block_height, u64 first_byte) {
    if (output.empty() || row_bytes == 0 || height == 0) return;
    const u64 gobs_x = (u64{row_bytes} + 63) / 64;
    const u64 column_bytes = 512ULL << block_height;
    const u64 block_bytes = gobs_x * column_bytes;
    const u64 end = first_byte + output.size();
    for (u64 gob = first_byte & ~u64{511}; gob < end; gob += 512) {
        const u64 block_y = gob / block_bytes;
        const u64 in_block = gob % block_bytes;
        const u64 x_base = (in_block / column_bytes) * 64;
        const u64 y_base = (block_y << (block_height + 3)) + (in_block % column_bytes) / 512 * 8;
        if (x_base >= row_bytes || y_base >= height) continue;
        for (u64 y = 0; y < 8 && y_base + y < height; ++y) {
            for (u64 x = 0; x < 64 && x_base + x < row_bytes; x += 16) {
                const u64 tiled = gob + (x / 32) * 256 + (y / 2) * 64 +
                                  ((x % 32) / 16) * 32 + (y % 2) * 16;
                const u64 lo = std::max(tiled, first_byte);
                const u64 hi = std::min({tiled + 16, tiled + row_bytes - x_base - x, end});
                if (lo >= hi) continue;
                const u64 linear = (y_base + y) * row_bytes + x_base + x + lo - tiled;
                std::memcpy(output.data() + lo - first_byte, input.data() + linear, hi - lo);
            }
        }
    }
}

} // namespace Tegra::Texture
