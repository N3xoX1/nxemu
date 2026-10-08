// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <bit>
#include <limits>
#include <unordered_map>

#include "yuzu_common/common_types.h"

namespace Vulkan {

// Updated by the GPU thread and disk-cache reader, never by pipeline worker jobs.
class CbufSizeDependencies {
public:
    void Observe(u64 shader, u32 mask) {
        if (shader != 0) {
            const auto [it, inserted] = masks.try_emplace(shader, mask);
            const u32 combined = it->second | mask;
            if (inserted || combined != it->second) {
                it->second = combined;
                ++revision;
            }
        }
    }

    u32 Mask(u64 shader) const {
        if (shader == 0) {
            return 0;
        }
        const auto it = masks.find(shader);
        // Until translation has reported its dependencies, specialize conservatively.
        return it != masks.end() ? it->second : std::numeric_limits<u32>::max();
    }

    u64 Revision() const noexcept { return revision; }

    template <size_t NumBanks>
    static void Normalize(std::array<u32, NumBanks>& sizes, u32 mask) {
        static_assert(NumBanks <= 32);
        for (size_t bank = 0; bank < NumBanks; ++bank) {
            if ((mask & (1U << bank)) == 0) {
                sizes[bank] = 0;
            }
        }
    }

private:
    std::unordered_map<u64, u32> masks;
    u64 revision{};
};

// Retain only dependency metadata, never bound sizes or guest addresses. Required banks
// are read on every draw, including enable/disable and size changes with the same shader.
template <size_t NumStages, size_t NumBanks>
class GraphicsCbufSizeState {
public:
    GraphicsCbufSizeState() { stage_masks.fill(std::numeric_limits<u32>::max()); }

    template <typename ReadSize>
    void Refresh(std::array<std::array<u32, NumBanks>, NumStages>& sizes,
                 const std::array<u64, NumStages + 1>& hashes,
                 const CbufSizeDependencies& dependencies, ReadSize&& read_size) {
        static_assert(NumBanks < 32);
        if (revision != dependencies.Revision() || shader_hashes != hashes) {
            shader_hashes = hashes;
            revision = dependencies.Revision();
            for (size_t stage = 0; stage < NumStages; ++stage) {
                const u32 mask = (dependencies.Mask(hashes[stage + 1]) |
                    (stage == 0 ? dependencies.Mask(hashes[0]) : 0)) & ((1U << NumBanks) - 1);
                if (mask != stage_masks[stage]) {
                    sizes[stage].fill(0);
                    stage_masks[stage] = mask;
                }
            }
        }
        for (size_t stage = 0; stage < NumStages; ++stage) {
            u32 mask = stage_masks[stage];
            while (mask != 0) {
                const auto bank = std::countr_zero(mask);
                sizes[stage][bank] = read_size(stage, bank);
                mask &= mask - 1;
            }
        }
    }
private:
    std::array<u64, NumStages + 1> shader_hashes{};
    std::array<u32, NumStages> stage_masks{};
    u64 revision{std::numeric_limits<u64>::max()};
};

} // namespace Vulkan
