// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <filesystem>
#include <string>

namespace VideoCommon::ShaderProfile {
// libc++ can use a signed 128-bit file-clock representation. Keep every native
// tick without narrowing it or changing existing local index/receipt identities.
inline std::string FileTimestamp(std::filesystem::file_time_type time) {
    auto ticks = time.time_since_epoch().count();
    const bool negative = ticks < 0;
    std::string result;
    do {
        const auto digit = static_cast<int>(ticks % 10);
        result.push_back(static_cast<char>('0' + (negative ? -digit : digit)));
        ticks /= 10;
    } while (ticks != 0);
    if (negative) result.push_back('-');
    std::reverse(result.begin(), result.end());
    return result;
}
} // namespace VideoCommon::ShaderProfile
