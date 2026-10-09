// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <limits>
#include <utility>

#include "yuzu_common/common_types.h"

namespace Vulkan {

// Lives for one shader stage of one draw/dispatch. Never retains guest pointers across draws,
// rebindings or address-space changes. Discontiguous ranges keep the original scalar read path.
template <size_t NumBanks>
class DescriptorCbufReader {
public:
    template <typename Memory>
    struct BoundBuffer {
        Memory* memory;
        GPUVAddr address;
        u32 size;
        const u8* span;
        u32 Read(u32 offset) const {
            if (span && offset <= size - sizeof(u32)) {
                u32 value;
                std::memcpy(&value, span + offset, sizeof(value));
                return value;
            }
            return memory->template Read<u32>(address + offset);
        }
    };

    // Prepare once per descriptor array. A fragmented bank takes the scalar path directly,
    // without repeating bank/size/span queries for every array element.
    template <typename Memory>
    BoundBuffer<Memory> Bind(Memory& memory, u32 bank, GPUVAddr address, u32 size, u32 count) {
        const u8* span{};
        if (count > 64 && bank < NumBanks && size >= sizeof(u32)) {
            const u32 bit = 1U << bank;
            if ((queried_banks & bit) == 0) {
                spans[bank] = memory.GetSpan(address, size);
                queried_banks |= bit;
            }
            span = spans[bank];
        }
        return {&memory, address, size, span};
    }

    template <typename Memory>
    u32 Read(Memory& memory, u32 bank, GPUVAddr address, u32 size, u32 offset, u32 count) {
        return Bind(memory, bank, address, size, count).Read(offset);
    }

private:
    static_assert(NumBanks <= 32);
    std::array<const u8*, NumBanks> spans; // Only read after its queried bit is set.
    u32 queried_banks{};
};

// Reset after synchronizing descriptor tables, before each draw/dispatch. Caches lookups only
// within that operation, so pool modifications and sampler rebindings always remain visible.
template <typename Value, size_t NumSlots = 256>
class DrawDescriptorCache {
public:
    DrawDescriptorCache() { Reset(); }

    void Reset() {
        consecutive_misses = 0;
        enabled = cooldown == 0;
        if (!enabled) {
            --cooldown;
            return;
        }
        indices.fill(std::numeric_limits<u32>::max());
    }

    bool Enabled() const noexcept { return enabled; }

    template <typename Lookup>
    Value Get(u32 index, Lookup&& lookup) {
        if (!enabled || index == std::numeric_limits<u32>::max()) {
            return lookup(index);
        }
        const size_t slot = index & (NumSlots - 1);
        if (indices[slot] != index) {
            values[slot] = lookup(index);
            indices[slot] = index;
            if (++consecutive_misses >= 64) {
                // Correctness is unchanged: bypass useless caching and probe again later.
                enabled = false;
                cooldown = 16;
            }
        } else {
            consecutive_misses = 0;
        }
        return values[slot];
    }

private:
    static_assert(NumSlots != 0 && (NumSlots & (NumSlots - 1)) == 0);
    u32 consecutive_misses{}, cooldown{};
    bool enabled{true};
    std::array<u32, NumSlots> indices{};
    std::array<Value, NumSlots> values{};
};

} // namespace Vulkan
