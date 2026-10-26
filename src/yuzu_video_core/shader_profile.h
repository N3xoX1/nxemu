// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
#include <functional>
#include <set>
#include <map>
#include "yuzu_common/common_types.h"
#include "yuzu_video_core/shared_shader_bytes.h"

namespace VideoCommon::ShaderProfile {
namespace Timing { struct Metrics; }
using ResourceProgress = std::function<bool(size_t,size_t)>;
struct Recipe {
    bool compute{}, requires_validation{};
    // source_context: the version this recipe is used as, chosen at import among
    // source_contexts, every version the recipe was recorded with.
    std::string source_context, identity;
    std::vector<std::string> source_contexts;
    std::vector<u32> guest_words;
    std::array<u64,6> shader_hashes{};
    std::array<std::array<u32,18>,5> cbuf_sizes{};
    std::array<u32,2> texture_pool{};
    u32 shared_memory_size{};
    std::array<u32,3> workgroup_size{};
    std::array<u32,8> compute_cbuf_sizes{};
    u32 texture_table_size{};
    std::vector<SharedShaderBytes> environments;
};
// Local program bytes keyed by every hash the emulator can give them.
using ProgramMap = std::map<u64,std::shared_ptr<const std::string>>;
struct ImportReport {
    size_t candidates{}, accepted{}, reused{}, skipped{}, missing_resources{}, incompatible_resources{}, incompatible_contexts{};
    std::vector<std::string> reused_ids;
};
// A game version a profile was recorded with: base and update version numbers
// (update empty without one). Informative: other versions may still match.
struct GameVersion {
    std::string context,base,update;
};
inline constexpr size_t MaxVersionText=32;
// "1.0.0" or "1.0.0 + update 4.0.1"; empty when unknown.
inline std::string GameVersionText(const GameVersion& version) {
    if(version.base.empty()&&version.update.empty())return {};
    return (version.base.empty()?std::string("?"):version.base)+(version.update.empty()?std::string{}:" + update "+version.update);
}
struct ProfileInspection {
    size_t graphics{},compute{},unresolved{},bytes{},programs{},environments{};
    size_t versions{}; // Distinct game versions the recipes were recorded with.
    std::string label,revision,note;
    bool full_game{}; // The author recorded the whole game.
    std::vector<GameVersion> targets; // The versions the recipes were recorded with.
    std::string primary_context;
    std::vector<std::string> contexts;
};
// Structure/title validation only. Reads no game resources and creates no pipelines.
ProfileInspection Inspect(const std::filesystem::path& profile,u64 title_id);
// Startup only. Select readable versioned exports using their verified payload,
// never a version number inferred from their filenames. Legacy names remain valid.
std::filesystem::path FindProfileForContext(const std::vector<std::filesystem::path>& directories,
                                          u64 title_id,std::string_view context);
// Only metadata is accepted from the profile; all instruction bytes come from game_root.
std::vector<Recipe> Read(const std::filesystem::path& path,
                         const std::filesystem::path& game_root, u64 title_id,
                         size_t* unresolved_count = nullptr, Timing::Metrics* timings = nullptr,
                         const ResourceProgress& progress = {}, ImportReport* report = nullptr, std::string_view target_context = {}, const std::set<std::string>* existing_recipes = nullptr,
                         const ProgramMap* existing_programs = nullptr, bool allow_cross_version = false);
} // namespace VideoCommon::ShaderProfile
