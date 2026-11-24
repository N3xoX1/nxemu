// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <optional>
#include <span>
#include <utility>
#include <vector>
#include "yuzu_common/common_types.h"

// NXShader stores Switch 1 guest state as the guest itself defines it, never
// in an emulator's own layout: Maxwell 3D (class B197) register values for the
// pipeline state, texture descriptor (TIC) fields for texture queries.
namespace VideoCommon::ShaderProfile::GuestState
{
// Keys past the register file: what a draw adds to the registers.
inline constexpr u32 TopologyKey = 0x10000;   // Maxwell 3D primitive topology of the draw.
inline constexpr u32 EngineHintKey = 0x10001; // 1: the draw comes from a high-level macro.
inline constexpr u32 RegisterCount = 0xE00;
// Sorted by key, values never 0: a key that is absent is 0.
using Registers = std::vector<std::pair<u32, u32>>;
// The registers of a recipe's guest words (the in-memory 278-word state).
// nullopt when no register set gives these words back.
std::optional<Registers> FromWords(std::span<const u32> words);
// The canonical guest words of a register set; nullopt for an invalid set.
std::optional<std::vector<u32>> ToWords(const Registers & registers);

// Texture type: TIC texture type (bits 0-3) | normalized coordinates (bit 4).
u32 TextureTypeCode(u32 shader_texture_type);
std::optional<u32> ShaderTextureType(u32 code);
// Texture format: TIC word 0 bits 0-18 (format, then red, green, blue and
// alpha component types) | sRGB (bit 19).
std::optional<u32> TextureFormatCode(u32 pixel_format);
std::optional<u32> PixelFormat(u32 code);
} // namespace VideoCommon::ShaderProfile::GuestState
