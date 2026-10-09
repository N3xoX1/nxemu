// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstring>
#include <span>
#include "system_loader.h"

namespace ApplicationControlData {

inline constexpr uint32_t NacpSize = 0x4000;
inline constexpr uint32_t MaxIconSize = 0x20000;
inline constexpr uint32_t MaxSize = NacpSize + MaxIconSize;

// A null, zero-sized buffer queries the size. Other calls copy the complete payload.
// The reader must report the number of bytes actually read, never its requested size.
template <typename IconReader>
LoaderResultStatus Read(std::span<const uint8_t> nacp, uint64_t icon_size,
                        IconReader&& read_icon, uint8_t* buffer, uint32_t buffer_size,
                        uint32_t& actual_size) {
    actual_size = 0;
    if (nacp.size() != NacpSize) {
        return LoaderResultStatus::ErrorNoControl;
    }
    if (icon_size > MaxIconSize) {
        return LoaderResultStatus::ErrorNoIcon;
    }
    const auto size = NacpSize + static_cast<uint32_t>(icon_size);
    if (buffer == nullptr && buffer_size == 0) {
        actual_size = size;
        return LoaderResultStatus::Success;
    }
    if (buffer == nullptr || buffer_size < size) {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    if (icon_size != 0 && read_icon(buffer + NacpSize, icon_size) != icon_size) {
        return LoaderResultStatus::ErrorNoIcon;
    }
    std::memcpy(buffer, nacp.data(), NacpSize);
    actual_size = size;
    return LoaderResultStatus::Success;
}

} // namespace ApplicationControlData
