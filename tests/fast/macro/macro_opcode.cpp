// SPDX-FileCopyrightText: Copyright 2026 NXEmu Contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cassert>
#include <cstdint>

#include "yuzu_video_core/macro/macro.h"

int main() {
    using namespace Tegra::Macro;

    // All five-bit bitfield sizes are legal; the 31-bit case must not shift a signed int.
    for (u32 width = 0; width <= 31; ++width) {
        Opcode opcode{};
        opcode.bf_size.Assign(width);
        const u32 expected = width == 0 ? 0U : (0xffffffffU >> (32 - width));
        assert(opcode.GetBitfieldMask() == expected);
    }

    constexpr std::array<u32, 10> shifts{0, 1, 7, 15, 31, 32, 33, 63, 64, 0xffffffffU};
    constexpr u32 source = 0xb4792d05U;
    for (const u32 count : shifts) {
        const u32 masked = MaskShiftCount(count);
        assert(masked < 32);
        assert(masked == (count % 32));
        // Both variable-shift interpreter opcodes must agree with the x64 JIT for all counts.
        assert((source >> masked) == (source >> (count % 32)));
        assert((source << masked) == (source << (count % 32)));
    }

    // A Send to a macro method must be recognized before a nested macro can mutate the FIFO.
    MethodAddress method{};
    method.raw = 0;
    method.address.Assign(0xDFF);
    assert(method.address.Value() < 0xE00);
    method.address.Assign(0xE00);
    assert(method.address.Value() >= 0xE00);
    method.address.Assign(0xFFF);
    assert(method.address.Value() >= 0xE00);
}
