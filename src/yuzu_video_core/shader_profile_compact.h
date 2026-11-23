// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "yuzu_video_core/shader_profile.h"
#include "yuzu_video_core/shader_profile_limits.h"
#include <cstdio>
class JsonValue;
namespace VideoCommon::ShaderProfile
{
// Locators per program: one per game version that contains it, tried in order.
inline constexpr size_t MaxProgramLocators = 8;
// What one profile may hold. Export drops the oldest game versions to fit.
struct ProfileLimits
{
    size_t recipes = MaxRecipes, unresolved = 65536, programs = 65536, environments = 262144, map_entries = 8000000;
    u64 code_bytes = 512 * 1024 * 1024;
};
// The NXShader payload (docs/nxshader-format.md): tagged sections of bounded
// little-endian tables, in the NXSHADR1 container. Resources name NCAs and count
// from their start, so a profile works with an NSP or an XCI of the same game
// version. Programs are named by the SHA-256 of their bytes, guest state by
// Maxwell registers and texture descriptor fields: nothing of one emulator.
ProfileInspection InspectCompact(std::string_view payload, u64 title_id);
std::string PackCompact(const JsonValue & profile, int compression_level = 19);
// The inverse of PackCompact on a checked payload, for merging profiles. Program
// keys are ProgramKey(code) since no instruction bytes are available.
JsonValue DecodeCompact(std::string_view payload, u64 title_id);
// The identity NXShader stores for program bytes: the first 8 bytes of their
// SHA-256. Enough to tell apart the programs of a game and to check the bytes read.
inline constexpr size_t ProgramIdentityBytes = 8;
std::string ProgramIdentity(std::string_view code);
// The model key DecodeCompact gives these bytes: that identity in hex, then zeros.
std::string ProgramKey(std::string_view code);
// The shader hash a decoded model gives a program key.
u64 ProgramKeyHash(std::string_view key);
std::vector<Recipe> ReadCompact(std::string_view payload, const std::filesystem::path & root,
                                u64 title_id, size_t * unresolved_count, Timing::Metrics * timings,
                                const ResourceProgress & progress = {}, ImportReport* report = nullptr, std::string_view target_context = {}, const std::set<std::string>* existing_recipes = nullptr,
                         const ProgramMap* existing_programs = nullptr, bool allow_cross_version = false);
} // namespace VideoCommon::ShaderProfile
