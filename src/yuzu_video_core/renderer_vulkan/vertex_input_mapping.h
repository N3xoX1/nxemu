// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <numeric>

namespace Vulkan {

// Guest slot numbers are independent of the number of inputs consumed by a draw.
// Compact only live inputs, preserving shared streams and their instancing divisors.
struct VertexInputMapping {
    static constexpr uint32_t UNUSED = ~uint32_t{};
    std::array<uint32_t, 32> locations{};
    std::array<uint32_t, 32> bindings{};
    uint32_t num_attributes{};
    uint32_t num_bindings{};

    VertexInputMapping() {
        std::iota(locations.begin(), locations.end(), 0U);
        std::iota(bindings.begin(), bindings.end(), 0U);
    }

    VertexInputMapping(const std::array<uint32_t, 32>& streams, uint32_t used_mask)
        : VertexInputMapping() {
        locations.fill(UNUSED);
        bindings.fill(UNUSED);
        for (uint32_t index = 0; index < locations.size(); ++index) {
            if ((used_mask & (uint32_t{1} << index)) == 0) {
                continue;
            }
            locations[index] = num_attributes++;
            auto& binding = bindings.at(streams[index]);
            if (binding == UNUSED) {
                binding = num_bindings++;
            }
        }
    }
};

template <class State, class Loads>
VertexInputMapping MakeVertexInputMapping(const State& state, const Loads& loads) {
    std::array<uint32_t, 32> streams{};
    uint32_t used_mask{};
    for (uint32_t index = 0; index < streams.size(); ++index) {
        streams[index] = state.attributes[index].buffer;
        if (state.attributes[index].enabled && loads.Generic(index)) {
            used_mask |= uint32_t{1} << index;
        }
    }
    return VertexInputMapping{streams, used_mask};
}

} // namespace Vulkan
