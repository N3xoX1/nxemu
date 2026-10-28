// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/json.h"
#include "yuzu_common/common_types.h"
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>

namespace VideoCommon::ShaderProfile
{
// All callbacks execute on the caller's offline worker, never on a draw thread.
using Progress = std::function<bool(const char * phase, u64 completed, u64 total)>;
struct ExportOptions
{
    // previous: the installed profile, kept first in the export. Optional.
    std::filesystem::path cache, recording, game_root, output, index, previous;
    std::string base, update, label, context, revision, note;
    u64 title_id{};
    bool allow_partial{}, compact{}, automatic_sources{};
    bool full_game{}; // The author recorded the whole game.
    int compression_level{19}; // Offline export only.
    Progress progress;
};
struct ExportReport
{
    u64 programs{}, environments{}, graphics{}, compute{}, unresolved{}, missing{};
    u64 reused{}, searched_resources{}, decoded_bytes{}, bytes{};
    u64 resolved_hle_aliases{}; // Observations recovered via a replacement-free HLE source.
    u64 inherited{}, versions{}; // Recipes kept from the previous profile; game versions covered.
    enum class Previous { None, Merged, Unreadable } previous{};
    u64 dropped_recipes{}, dropped_versions{}; // Older game versions removed to fit the limits.
    double seconds{};
};
// Throws on cancellation/error; publication is atomic and never replaces an output.
ExportReport Export(const ExportOptions & options);
// Environment ids from content only (sorted maps, no header digest), with
// shader keys renamed by `shaders`; environments of `dropped` shaders and the
// recipes using them are removed.
void CanonicalizeEnvironments(JsonValue & profile, const std::map<std::string, std::string> & shaders = {},
                              const std::set<std::string> & dropped = {});
// Puts `previous` (a DecodeCompact result) before `current` (canonical), each
// recipe keeping its game version. Returns how many previous recipes were kept.
u64 MergeProfiles(JsonValue previous, JsonValue & current);
struct ProfileLimits;
struct TrimReport
{
    u64 recipes{}, versions{};
};
// Removes programs and environments no recipe uses, then, while the profile
// exceeds `limits`, the oldest game version other than `keep` (the order in
// which versions first appear) with the recipes recorded only with it.
TrimReport TrimToLimits(JsonValue & profile, const std::string & keep, const ProfileLimits & limits);
// Stable ASCII, sorted-key JSON for offline diagnostic and local metadata.
std::string Canonical(const JsonValue & value);
JsonValue ParseMetadata(std::string_view bytes, size_t limit = 32 * 1024 * 1024);
void Publish(const std::filesystem::path & output, std::string_view bytes);
} // namespace VideoCommon::ShaderProfile
