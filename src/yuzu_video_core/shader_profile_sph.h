// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_shader_recompiler/program_header.h"
#include <array>
#include <cstring>
namespace VideoCommon::ShaderProfile
{
// Canonical guest wire words, not a subset of currently observed compiler reads.
// The named NOINIT padding in ProgramHeader denotes guest header bytes, not
// host padding. Preserve reserved bits until ignoring them is proven safe.
inline std::array<u32, 20> CanonicalGuestHeader(const Shader::ProgramHeader & header) noexcept
{
    static_assert(sizeof(header) == 80);
    std::array<unsigned char, 80> wire{};
    std::memcpy(wire.data(), &header, wire.size());
    std::array<u32, 20> words{};
    for (size_t i = 0; i < words.size(); ++i)
        for (size_t byte = 0; byte < 4; ++byte)
            words[i] |= u32(wire[i * 4 + byte]) << (8 * byte);
    return words;
}
} // namespace VideoCommon::ShaderProfile
