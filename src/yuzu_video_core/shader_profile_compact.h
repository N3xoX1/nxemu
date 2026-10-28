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
// NXSHBIN1 only. Bounded little-endian tables in the NXSHADR2 Zstd envelope.
// Resources name NCAs and count from their start, so a profile works with an
// NSP or an XCI of the same game version.
// Programs carry their emulator hash; resolved recipes derive theirs from them.
// No legacy payload readers, format migration or compatibility branches.
ProfileInspection InspectCompact(std::string_view payload, u64 title_id);
std::string PackCompact(const JsonValue & profile, int compression_level = 19);
// The inverse of PackCompact on a checked payload, for merging profiles. Program
// keys are ProgramKey(hash, size) since no instruction bytes are available.
JsonValue DecodeCompact(std::string_view payload, u64 title_id);
inline std::string ProgramKey(u64 hash, u64 size)
{
    char key[65];
    std::snprintf(key, sizeof(key), "%016llx%016llx%032x", static_cast<unsigned long long>(hash),
                  static_cast<unsigned long long>(size), 0U);
    return key;
}
std::vector<Recipe> ReadCompact(std::string_view payload, const std::filesystem::path & root,
                                u64 title_id, size_t * unresolved_count, Timing::Metrics * timings,
                                const ResourceProgress & progress = {}, ImportReport* report = nullptr, std::string_view target_context = {}, const std::set<std::string>* existing_recipes = nullptr,
                         const ProgramMap* existing_programs = nullptr, bool allow_cross_version = false);
} // namespace VideoCommon::ShaderProfile
