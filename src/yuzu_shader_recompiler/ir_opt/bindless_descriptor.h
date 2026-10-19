// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_common/common_types.h"

namespace Shader::Optimization {
// A selected handle field must fit entirely in the bound constant buffer.
constexpr u32 BindlessDescriptorCount(u32 buffer_size, u32 base_offset, u32 size_shift) {
    constexpr u32 word_size = static_cast<u32>(sizeof(u32));
    if (size_shift > 31 || base_offset > buffer_size || buffer_size - base_offset < word_size) {
        return 0;
    }
    return 1 + (buffer_size - base_offset - word_size) / (1U << size_shift);
}
}
