// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_shader_recompiler/program_header.h"
#include <algorithm>
#include <array>
#include <cstring>
namespace VideoCommon
{
/// A mask over the dynamic vertex attribute types of a pipeline key (two bits per attribute) with
/// both bits set for each generic input the vertex program header declares
[[nodiscard]] inline u64 DeclaredInputTypeMask(const Shader::ProgramHeader& sph) noexcept {
    u64 mask{};
    for (u32 attribute = 0; attribute < 32; ++attribute) {
        if (std::ranges::any_of(sph.vtg.InputGeneric(attribute), [](bool used) { return used; })) {
            mask |= u64{3} << (attribute * 2);
        }
    }
    return mask;
}
} // namespace VideoCommon

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
