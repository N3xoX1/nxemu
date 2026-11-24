// SPDX-License-Identifier: GPL-2.0-or-later
// nxshader_tool basepack: a base shader pack for a game that keeps its shaders in Nintendo's formats.
// It holds a short recorded session and synthetic recipes for the program sets the game's files hold
// that the session never drew: each set gets the states most used with its programs in the session,
// most likely first (SyntheticSetBudget). A program the session never ran gets an environment made by
// translating it against the game files: its own constants, made-up texture handles (a draw re-reads
// them), and what the session shows of the whole game (texture bank, viewport, usual texture type).
// The first draw of a synthetic pipeline checks it, so a wrong guess costs one compile.
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "bfres_layouts.h"
#include "common/json.h"
#include "yuzu_shader_recompiler/environment.h"
#include "yuzu_shader_recompiler/frontend/maxwell/control_flow.h"
#include "yuzu_shader_recompiler/frontend/maxwell/translate_program.h"
#include "yuzu_shader_recompiler/host_translate_info.h"
#include "yuzu_shader_recompiler/object_pool.h"
#include "yuzu_video_core/shader_profile_codec.h"
#include "yuzu_video_core/shader_profile_compact.h"
#include "yuzu_video_core/shader_profile_discovery.h"
#include "yuzu_video_core/shader_profile_export.h"
#include "yuzu_video_core/shader_profile_harvest.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"

using namespace VideoCommon;
namespace fs = std::filesystem;

namespace {
u64 Num(const JsonValue& n) {
    return n.isString() ? std::stoull(n.asString()) : n.asUInt64();
}
std::string Read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return {(std::istreambuf_iterator<char>(in)), {}};
}
template <class T>
T Mode(const std::map<T, size_t>& counts, T fallback) {
    T best = fallback;
    size_t most = 0;
    for (const auto& [value, count] : counts)
        if (count > most) { best = value; most = count; }
    return best;
}

// Runs unchanged at another start address of the same alignment mod 32: no absolute jump or PC
// read outside scheduling words (the check the first draw makes, CanRebase, is stricter).
bool Relocatable(std::string_view code, u32 start) {
    for (size_t i = 10; i * 8 + 8 <= code.size(); ++i) {
        if ((start + i * 8) % 32 == 0) continue;
        u64 word;
        std::memcpy(&word, code.data() + i * 8, 8);
        const auto opcode = word >> 52;
        if (opcode == 0xe20 || opcode == 0xe21 || opcode == 0xe22 || opcode == 0x50d) return false;
    }
    return true;
}

// Answers a translation from the game files and records what it asked.
class StaticEnvironment final : public Shader::Environment {
public:
    StaticEnvironment(std::string_view code_, Shader::Stage stage_, u32 start, std::string_view constants_, u32 bank_,
                      u32 viewport_, Shader::TextureType type_)
        : code{code_}, constants{constants_}, bank{bank_}, viewport{viewport_}, type{type_} {
        stage = stage_;
        start_address = start;
        is_proprietary_driver = bank == 2;
        if (stage != Shader::Stage::Compute) {
            if (code.size() < sizeof(sph)) throw std::runtime_error("program shorter than its header");
            std::memcpy(&sph, code.data(), sizeof(sph));
        }
        const u64 local = sph.LocalMemorySize() + sph.common3.shader_local_memory_crs_size;
        if (local > UINT32_MAX) throw std::runtime_error("local memory");
        local_memory = static_cast<u32>(local);
    }
    u64 ReadInstruction(u32 address) override {
        if (address < start_address || address - start_address + 8 > code.size())
            throw std::runtime_error("read outside the program");
        u64 word;
        std::memcpy(&word, code.data() + (address - start_address), 8);
        return word;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        const u64 key = (u64{index} << 32) | offset;
        u32 value{};
        if (index == bank) {
            // A texture handle: made up from its slot, so that each slot names its own texture.
            if (!in_query) throw std::runtime_error("texture bank read outside a texture query");
            value = (offset / 4 + 1) & 0xfffff;
        } else if (index == 1) {
            if (offset % 4 || offset + 4 > constants.size()) throw std::runtime_error("constant outside the program's own");
            std::memcpy(&value, constants.data() + offset, 4);
        } else {
            throw std::runtime_error("constant bank " + std::to_string(index));
        }
        cbuf_values[key] = value;
        return value;
    }
    u32 ReadTextureQuery(const Shader::TextureHandleSource& source, Shader::TextureQueryKind kind) override {
        in_query = true;
        try {
            const auto answer = Shader::Environment::ReadTextureQuery(source, kind);
            in_query = false;
            return answer;
        } catch (...) {
            in_query = false;
            throw;
        }
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        texture_types[handle] = type;
        return type;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        throw std::runtime_error("image format");
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        return static_cast<u32>(ReadTexturePixelFormat(handle)) != 0;
    }
    u32 ReadViewportTransformState() override { return viewport; }
    u32 TextureBoundBuffer() const override { return bank; }
    u32 LocalMemorySize() const override { return local_memory; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

    // The environment as a profile model holds it.
    JsonValue Model(const std::string& key) const {
        JsonValue env;
        env["cbuf_replacements"] = JsonValue(JsonValueType::Array);
        env["cbuf_values"] = JsonValue(JsonValueType::Array);
        for (const auto& [k, v] : cbuf_values) {
            JsonValue pair(JsonValueType::Array);
            pair.Append(JsonValue(std::to_string(k)));
            pair.Append(JsonValue(v));
            env["cbuf_values"].Append(std::move(pair));
        }
        env["guest_read_highest"] = static_cast<u32>(start_address + code.size() - 8);
        env["guest_read_lowest"] = start_address;
        env["guest_start_address"] = start_address;
        env["local_memory_size"] = local_memory;
        env["shader"] = key;
        env["stage"] = static_cast<u32>(stage);
        env["texture_bound"] = bank;
        env["texture_pixel_formats"] = JsonValue(JsonValueType::Array);
        env["texture_types"] = JsonValue(JsonValueType::Array);
        for (const auto& [h, t] : texture_types) {
            JsonValue pair(JsonValueType::Array);
            pair.Append(JsonValue(h));
            pair.Append(JsonValue(static_cast<u32>(t)));
            env["texture_types"].Append(std::move(pair));
        }
        env["viewport_transform_state"] = viewport;
        return env;
    }

private:
    std::string_view code, constants;
    u32 bank, viewport, local_memory{};
    Shader::TextureType type;
    bool in_query{};
    std::map<u64, u32> cbuf_values;
    std::map<u32, Shader::TextureType> texture_types;
};
} // namespace

// basepack <game folder> <base> <update|-> <title> <session.nxshader> <output.nxshader>
int BasePack(int argc, char** argv) {
    if (argc != 8) throw std::runtime_error("basepack <game folder> <base> <update|-> <title> <session> <output>");
    ShaderProfile::ExportOptions options;
    options.game_root = fs::u8path(argv[2]);
    options.base = argv[3];
    if (std::string_view(argv[4]) != "-") options.update = argv[4];
    options.title_id = std::stoull(argv[5], nullptr, 16);
    // States written per program set, and per model layout.
    constexpr size_t per_set = 4, model_states = 2;
    auto profile = ShaderProfile::DecodeCompact(ShaderProfile::Unpack(Read(fs::u8path(argv[6]))), options.title_id);
    // The model files are read as the harvest walks the game: their vertex layouts by program set.
    BfresLayouts::Collector models;
    const auto harvest = ShaderProfile::HarvestPrograms(options, [&](std::string_view data, const std::string& origin) { models.Add(data, origin); });
    const auto layouts = models.Finish();
    size_t layout_count{};
    for (const auto& [set, list] : layouts) layout_count += list.size();
    std::cout << "models: " << layouts.size() << " program sets with " << layout_count << " vertex layouts\n";
    for (const auto& [what, count] : models.stats) std::cout << "  " << count << "  " << what << '\n';

    // What the session shows: the programs, their environments and states, and facts of the whole game.
    const auto& envs = profile["environments"];
    std::map<std::string, std::map<std::string, size_t>> env_uses;  // program -> environment -> recipes
    std::map<std::string, std::map<size_t, size_t>> state_uses;      // program -> state -> recipes
    std::map<std::string, size_t> state_of;                          // guest words -> state
    std::vector<size_t> state_recipe;                                // state -> a recipe that holds it
    std::set<std::vector<std::string>> drawn;
    std::map<u32, size_t> banks, viewports, starts, types;
    for (const auto& id : envs.GetMemberNames()) {
        const auto& e = envs[id];
        banks[static_cast<u32>(Num(e["texture_bound"]))]++;
        if (Num(e["stage"]) != static_cast<u32>(Shader::Stage::Fragment) && Num(e["stage"]) != static_cast<u32>(Shader::Stage::Compute))
            viewports[static_cast<u32>(Num(e["viewport_transform_state"]))]++;
        if (Num(e["stage"]) != static_cast<u32>(Shader::Stage::Compute)) starts[static_cast<u32>(Num(e["guest_start_address"]) % 32)]++;
        for (const auto& t : e["texture_types"]) types[static_cast<u32>(Num(t[1]))]++;
    }
    const auto& recipes = profile["recipes"];
    for (u32 i = 0; i < recipes.size(); ++i) {
        const auto& r = recipes[i];
        if (r["kind"].asString() != "graphics") continue;
        const auto words = ShaderProfile::Canonical(r["guest_words"]);
        const auto [found, added] = state_of.try_emplace(words, state_recipe.size());
        if (added) state_recipe.push_back(i);
        std::vector<std::string> keys;
        for (const auto& id : r["environments"]) {
            const auto key = envs[id.asString()]["shader"].asString();
            keys.push_back(key);
            ++env_uses[key][id.asString()];
            ++state_uses[key][found->second];
        }
        std::sort(keys.begin(), keys.end());
        drawn.insert(std::move(keys));
    }
    const u32 bank = Mode(banks, 2U), viewport = Mode(viewports, 1U), start32 = Mode(starts, 16U);
    const auto type = static_cast<Shader::TextureType>(Mode(types, static_cast<u32>(Shader::TextureType::Color2D)));
    std::cout << "session: " << recipes.size() << " recipes, " << state_recipe.size() << " states, " << env_uses.size()
              << " programs; game facts: texture bank " << bank << ", viewport " << viewport << ", start mod 32 " << start32
              << ", texture type " << static_cast<u32>(type) << '\n';
    std::cout << "harvest: " << harvest.programs.size() << " programs, " << harvest.groups.size() << " program sets\n";

    Shader::HostTranslateInfo host{};
    host.support_float64 = host.support_float16 = host.support_int64 = true;
    host.support_snorm_render_buffer = host.support_viewport_index_layer = true;
    host.min_ssbo_alignment = 16;
    host.support_geometry_shader_passthrough = host.support_conditional_barrier = true;
    host.max_storage_images_per_stage = 8;
    host.support_null_descriptor = host.support_separate_sampler = true;

    std::map<std::string, size_t> outcome;
    std::map<std::string, std::optional<std::string>> made;  // program -> environment id, or none
    std::map<std::string, std::string> codes;                // programs new to the profile, for Discover
    const auto environment = [&](const std::string& key, const ShaderProfile::HarvestedProgram& program) -> std::optional<std::string> {
        if (const auto found = env_uses.find(key); found != env_uses.end()) {
            std::string best;
            size_t most = 0;
            for (const auto& [id, count] : found->second)
                if (count > most) { best = id; most = count; }
            return best;
        }
        if (const auto found = made.find(key); found != made.end()) return found->second;
        auto& result = made[key];
        const auto stage = static_cast<Shader::Stage>(program.stage - 1);
        const u32 start = 0x100000 + start32;
        if (!Relocatable(program.code, start)) {
            ++outcome["program not relocatable"];
            return result;
        }
        try {
            StaticEnvironment env(program.code, stage, start, program.constants, bank, viewport, type);
            Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_blocks;
            Shader::ObjectPool<Shader::IR::Inst> insts;
            Shader::ObjectPool<Shader::IR::Block> blocks;
            Shader::Maxwell::Flow::CFG cfg(env, flow_blocks, start + static_cast<u32>(sizeof(Shader::ProgramHeader)));
            [[maybe_unused]] const auto translated = Shader::Maxwell::TranslateProgram(insts, blocks, env, cfg, host);
            auto model = env.Model(key);
            const auto id = "synthetic:" + key;
            profile["environments"][id] = std::move(model);
            profile["shaders"][key]["size"] = static_cast<u32>(program.code.size());
            profile["shaders"][key]["references"] = JsonValue(JsonValueType::Array);
            codes.emplace(key, program.code);
            ++outcome["environment made from the files"];
            result = id;
        } catch (const std::exception& error) {
            ++outcome[std::string("translation stopped: ") + error.what()];
        }
        return result;
    };

    // Synthetic recipes, set by set, most used state first.
    JsonValue synthetic(JsonValueType::Array);
    size_t sets{};
    for (const auto& group : harvest.groups) {
        auto sorted = group;
        std::sort(sorted.begin(), sorted.end());
        if (drawn.contains(sorted)) { ++outcome["set drawn in the session"]; continue; }
        bool usable = true;
        std::map<size_t, size_t> score;
        for (const auto& key : group) {
            const auto& program = harvest.programs.at(key);
            // Vertex A pairs, geometry (its passthrough mask is draw state) and compute are left out.
            usable &= program.platform == ShaderProfile::Harvest::Switch1 && program.stage >= 1 && program.stage <= 5 && program.stage != 4;
            if (const auto found = state_uses.find(key); found != state_uses.end())
                for (const auto& [state, count] : found->second) score[state] += count;
        }
        if (!usable) { ++outcome["set with stages left out"]; continue; }
        if (score.empty()) { ++outcome["set with no program in the session"]; continue; }
        std::vector<std::string> ids;
        std::array<u64, 6> hashes{};
        u32 previous_stage = 0;
        for (const auto& key : group) {
            const auto& program = harvest.programs.at(key);
            const auto id = environment(key, program);
            if (!id || program.stage <= previous_stage) { usable = false; break; }
            previous_stage = program.stage;
            ids.push_back(*id);
            hashes[program.stage] = ShaderProfile::ProgramKeyHash(key);
        }
        if (!usable) { ++outcome["set with a program left out"]; continue; }
        std::vector<std::pair<size_t, size_t>> ranked;
        for (const auto& [state, count] : score) ranked.emplace_back(count, state);
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        // States in rank order: with model layouts, the most used ones with each layout the model
        // files give the set; otherwise the most used ones as they are.
        std::vector<std::pair<size_t, std::optional<BfresLayouts::Layout>>> picks;
        if (const auto known = layouts.find(sorted); known != layouts.end()) {
            ++outcome["set with model layouts"];
            for (size_t i = 0; i < ranked.size() && i < model_states; ++i)
                for (const auto& layout : known->second) picks.emplace_back(ranked[i].second, layout);
        } else {
            for (size_t i = 0; i < ranked.size() && i < per_set; ++i) picks.emplace_back(ranked[i].second, std::nullopt);
        }
        for (const auto& [state, layout] : picks) {
            JsonValue recipe = recipes[static_cast<u32>(state_recipe[state])];
            if (layout) {
                std::vector<u32> words;
                for (const auto& n : std::as_const(recipe)["guest_words"]) words.push_back(static_cast<u32>(Num(n)));
                words = BfresLayouts::Apply(std::move(words), *layout);
                if (!Vulkan::GuestPipelineState::FromWords(words)) { ++outcome["model layout the guest state refuses"]; continue; }
                recipe["guest_words"] = JsonValue(JsonValueType::Array);
                for (const auto w : words) recipe["guest_words"].Append(JsonValue(w));
            }
            recipe["environments"] = JsonValue(JsonValueType::Array);
            for (const auto& id : ids) recipe["environments"].Append(JsonValue(id));
            recipe["shader_hashes"] = JsonValue(JsonValueType::Array);
            for (const auto h : hashes) recipe["shader_hashes"].Append(h ? JsonValue(std::to_string(h)) : JsonValue(0U));
            recipe["synthetic"] = true;
            recipe.removeMember("played");
            synthetic.Append(std::move(recipe));
        }
        ++sets;
    }
    for (u32 i = 0; i < synthetic.size(); ++i) profile["recipes"].Append(std::as_const(synthetic)[i]);
    std::cout << "synthetic: " << synthetic.size() << " recipes for " << sets << " program sets, " << codes.size()
              << " programs new to the profile\n";
    for (const auto& [what, count] : outcome) std::cout << "  " << count << "  " << what << '\n';

    // Where the new programs lie in the game files, then the file itself.
    ShaderProfile::ExportReport report;
    ShaderProfile::Discover(options, profile, codes, report);
    size_t unlocated{};
    for (const auto& [key, code] : codes) unlocated += profile["shaders"][key]["references"].empty();
    std::cout << "programs located: " << codes.size() - unlocated << " of " << codes.size() << '\n';
    ShaderProfile::CanonicalizeEnvironments(profile);
    const auto bytes = ShaderProfile::PackCompact(profile, 19);
    // Checked and never over an existing file: a failed write is an error, not a pack.
    ShaderProfile::Publish(fs::u8path(argv[7]), bytes);
    const auto written = ShaderProfile::InspectCompact(ShaderProfile::Unpack(bytes), options.title_id);
    std::cout << "written: " << written.graphics << " graphics recipes (" << written.synthetic << " synthetic), "
              << written.programs << " programs, " << written.environments << " environments, " << bytes.size() << " bytes\n";
    return 0;
}
