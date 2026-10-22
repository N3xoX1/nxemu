// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace Tegra {
// CPU copies of GPU-written macro arguments may be stale. Only replace an
// indirect count when the arguments are authoritative and the draw range starts
// at zero, so the host and guest draw IDs and buffer offsets agree.
constexpr std::optional<std::uint32_t> KnownMultiDrawCount(bool parameters_dirty,
                                                          std::uint32_t first,
                                                          std::uint32_t end,
                                                          std::uint32_t count) {
    if (parameters_dirty || first != 0) {
        return std::nullopt;
    }
    return std::min(end, count);
}
} // namespace Tegra
