// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_export.h"
#include "yuzu_video_core/shader_profile_context.h"
#include "yuzu_video_core/shader_profile_limits.h"
#include "yuzu_video_core/shader_profile_provenance.h"
#include "yuzu_video_core/pipeline_cache_store.h"
#include "yuzu_video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "yuzu_video_core/shader_profile_codec.h"
#include "yuzu_video_core/shader_profile_compact.h"
#include "yuzu_video_core/shader_profile_discovery.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_journal.h"
#include "yuzu_video_core/shader_profile_resources.h"
#include "yuzu_video_core/shader_profile_text.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <utility>

namespace VideoCommon::ShaderProfile
{
namespace
{
void Require(bool ok, const char * reason)
{
    if (!ok) throw std::runtime_error(reason);
}
using Object = std::map<std::string, JsonValue>;
JsonValue Array()
{
    return JsonValue(JsonValueType::Array);
}
JsonValue Num(u64 n)
{
    return n > UINT32_MAX ? JsonValue(std::to_string(n)) : JsonValue(static_cast<u32>(n));
}
u64 Number(const JsonValue & n)
{
    return n.isString() ? std::stoull(n.asString()) : n.asUInt64();
}
struct Cursor
{
    std::string_view bytes;
    size_t pos{};
    std::string Take(size_t n)
    {
        Require(pos <= bytes.size() && n <= bytes.size() - pos, "Truncated local shader cache");
        auto s = std::string(bytes.substr(pos, n));
        pos += n;
        return s;
    }
    u64 Get(size_t n)
    {
        const auto s = Take(n);
        u64 v{};
        for (size_t i = 0; i < n; ++i)
            v |= u64{static_cast<unsigned char>(s[i])} << (8 * i);
        return v;
    }
};
std::string Load(const std::filesystem::path & p, size_t limit = 256 * 1024 * 1024)
{
    const auto size = std::filesystem::file_size(p);
    Require(size <= limit, "Offline input size limit");
    std::string bytes(static_cast<size_t>(size), '\0');
    std::ifstream in(p, std::ios::binary);
    in.read(bytes.data(), static_cast<std::streamsize>(size));
    Require(bool(in), "Cannot read offline input");
    return bytes;
}
struct Pipeline
{
    std::string key;
    std::vector<std::string> environments;
    bool compute{};
};
struct Model
{
    JsonValue profile{JsonValueType::Object};
    std::map<std::string, std::string> codes;
    std::vector<Pipeline> pipelines;
};
const std::array<const char *, 7> MetaNames{"local_memory_size", "texture_bound", "guest_start_address", "guest_read_lowest", "guest_read_highest", "viewport_transform_state", "stage"};
const std::array<const char *, 4> MapNames{"texture_types", "texture_pixel_formats", "cbuf_values", "cbuf_replacements"};

std::string ReadEnvironment(Cursor & input, Model & model)
{
    const auto begin = input.pos;
    const auto size = input.Get(8);
    std::array<u64, 4> counts;
    for (auto & n : counts)
    {
        n = input.Get(8);
        Require(n <= 65536, "Environment map limit");
    }
    Require(size && size % 8 == 0 && size <= 1024 * 1024, "Environment program size");
    JsonValue env(JsonValueType::Object);
    for (auto name : MetaNames)
        env[name] = Num(input.Get(4));
    const auto stage = Number(env["stage"]);
    Require(stage <= 6, "Invalid environment stage");
    Require(Number(env["guest_read_highest"]) >= Number(env["guest_read_lowest"]) &&
                Number(env["guest_read_highest"]) - Number(env["guest_read_lowest"]) + 8 == size,
            "Inconsistent shader bounds");
    auto code = input.Take(static_cast<size_t>(size));
    const auto digest = ResourceSha256(code);
    for (size_t i = 0; i < 4; ++i)
    {
        env[MapNames[i]] = Array();
        std::set<u64> keys;
        for (u64 n = 0; n < counts[i]; ++n)
        {
            const auto key = input.Get(i < 2 ? 4 : 8);
            Require(keys.insert(key).second, "Duplicate environment entry");
            JsonValue pair = Array();
            pair.Append(Num(key));
            pair.Append(Num(input.Get(4)));
            env[MapNames[i]].Append(std::move(pair));
        }
    }
    if (stage == 5)
    {
        env["workgroup_size"] = Array();
        for (unsigned i = 0; i < 3; ++i)
            env["workgroup_size"].Append(Num(input.Get(4)));
        env["shared_memory_size"] = Num(input.Get(4));
    }
    else
    {
        const auto header = input.Take(80);
        Require(code.size() >= 80 && header == code.substr(0, 80), "Shader header/code mismatch");
        env["program_header_sha256"] = ResourceSha256(header);
        if (stage == 3)
        {
            env["gp_passthrough_mask"] = Array();
            for (unsigned i = 0; i < 8; ++i)
                env["gp_passthrough_mask"].Append(Num(input.Get(4)));
        }
    }
    Require(input.pos > begin, "Empty environment");
    env["shader"] = digest;
    const auto id = ResourceSha256(Canonical(env));
    model.profile["environments"][id] = env;
    model.profile["shaders"][digest]["size"] = Num(size);
    model.profile["shaders"][digest]["references"] = Array();
    model.codes.emplace(digest, std::move(code));
    return id;
}
Model ReadCache(const ExportOptions & options)
{
    Model model;
    model.profile["environments"] = JsonValue(JsonValueType::Object);
    model.profile["shaders"] = JsonValue(JsonValueType::Object);
    std::ifstream source(options.cache,std::ios::binary);
    std::string magic(8,'\0');source.read(magic.data(),magic.size());
    Require(bool(source),"Truncated local cache header");
    if (magic == "NXSCACHE")
    {
        const auto contextual = options.context.empty() ? std::set<std::string>{} : CacheStore::ReadContext(options.cache, options.context);
        for (const auto & record : CacheStore::Read(options.cache, 20))
        {
            if (!options.context.empty() && !contextual.contains(record.id)) continue;
            Pipeline p;
            p.key = record.key;
            for (const auto & env : record.environments)
            {
                const auto serialized=env.String();
                Cursor c{serialized};
                p.environments.push_back(ReadEnvironment(c, model));
                Require(c.pos == env.size(), "Trailing environment data");
            }
            p.compute = Number(model.profile["environments"][p.environments.front()]["stage"]) == 5;
            Require(p.key.size() == (p.compute ? 64 : 1400), "Unsupported local pipeline ABI");
            model.pipelines.push_back(std::move(p));
        }
    }
    else throw std::runtime_error("Expected an NXEmu local shader store");
    Require(model.pipelines.size() <= MaxVersionPipelines, "Pipeline table limit");
    return model;
}
u64 At(const std::string & bytes, size_t pos, size_t count)
{
    Cursor c{bytes, pos};
    return c.Get(count);
}
void Put(std::string & bytes, size_t pos, u64 value, size_t count)
{
    Require(pos + count <= bytes.size(), "Pipeline key bounds");
    for (size_t i = 0; i < count; ++i)
        bytes[pos + i] = static_cast<char>(value >> (8 * i));
}
using Masks = std::map<u64, u32>;
std::string Normalize(std::string key, const Masks & dependencies)
{
    const auto mask = [&](u64 h) {const auto it=dependencies.find(h);return it==dependencies.end()?0:it->second; };
    auto state2 = At(key, 420, 4);
    if (!(state2 & (1 << 5))) Put(key, 420, state2 & ~u64(31 << 6), 4);
    bool pool = false;
    for (size_t stage = 0; stage < 5; ++stage)
    {
        const auto m = mask(At(key, 8 * (stage + 1), 8)) | (stage == 0 ? mask(At(key, 0, 8)) : 0);
        for (size_t bank = 0; bank < 18; ++bank)
            if (!(m & (1U << bank))) Put(key, 48 + (stage * 18 + bank) * 4, 0, 4);
    }
    for (size_t i = 0; i < 6; ++i)
        pool |= (mask(At(key, i * 8, 8)) & (1U << 31)) != 0;
    if (!pool) Put(key, 408, 0, 8);
    Vulkan::FixedPipelineState state;
    std::memcpy(&state, key.data() + 416, sizeof(state));
    return key.substr(0, 416 + state.Size());
}
// Compute keys keep only the constant buffer sizes their program reads.
std::string NormalizeCompute(std::string key, const Masks & dependencies)
{
    Require(key.size() == 64, "Unsupported compute key");
    const auto found = dependencies.find(At(key, 0, 8));
    const u32 mask = found == dependencies.end() ? 0 : found->second;
    for (size_t bank = 0; bank < 8; ++bank)
        if (!(mask & (1U << bank))) Put(key, 24 + bank * 4, 0, 4);
    Put(key, 60, 0, 4);
    return key;
}
JsonValue Assemble(Model & model, const ExportOptions & options, ExportReport & report)
{
    const auto recording = [&] {
        if (options.recording.extension() != ".nxr")
            return ParseMetadata(Load(options.recording, 32 * 1024 * 1024));
        auto failure_path = options.recording;
        failure_path += ".failed";
        Require(!std::filesystem::exists(failure_path), "Automatic guest recording failed; see renderer log");
        const auto journal = Journal::Read(options.recording);
        Require(!journal.incomplete_tail, "Incomplete recipe journal; start and stop the game to recover its final frame");
        JsonValue result(JsonValueType::Object);
        result["feature_bits"] = 0U;
        result["guest_state_word_count"] = 278U;
        result["records_compute"] = true;
        result["recipes"] = Array();
        for (const auto & entry : journal.entries)
        {
            JsonValue r(JsonValueType::Object);
            const bool compute = Journal::IsCompute(entry);
            if (compute) r["kind"] = "compute";
            else r["source_feature_bits"] = entry[0];
            r["words"] = Array();
            for (size_t i = 1; i < (compute ? 1 + Journal::ComputeWords : entry.size()); ++i)
                r["words"].Append(entry[i]);
            result["recipes"].Append(std::move(r));
        }
        return result;
    }();
    Require(!recording["failed"].asBool() && Number(recording["projection_mismatches"]) == 0 && Number(recording["dropped_observations"]) == 0, "Incomplete guest recipe recording");
    Require(recording.isMember("feature_bits") && Number(recording["feature_bits"]) <= 127 && Number(recording["guest_state_word_count"]) == 278, "Unsupported guest recording ABI");
    const auto bits = Number(recording["feature_bits"]);
    Masks dependencies;
    for (const auto & p : model.pipelines)
        if (p.compute)
        {
            for (const auto & entry : std::as_const(model.profile)["environments"][p.environments.front()]["cbuf_values"])
            {
                const auto key = Number(entry[0]), bank = key >> 32, offset = key & UINT32_MAX;
                if (offset == UINT32_MAX && bank < 8) dependencies[At(p.key, 0, 8)] |= 1U << bank;
            }
        }
        else
        {
            size_t stage = 0;
            for (size_t i = 0; i < 6; ++i)
                if (const auto hash = At(p.key, i * 8, 8))
                {
                    Require(stage < p.environments.size(), "Pipeline environment/hash mismatch");
                    const auto & env = model.profile["environments"][p.environments[stage++]];
                    for (const auto & entry : env["cbuf_values"])
                    {
                        const auto key = Number(entry[0]), bank = key >> 32, offset = key & UINT32_MAX;
                        if (offset == UINT32_MAX && bank < 32) dependencies[hash] |= 1U << bank;
                        if (key == UINT64_MAX - 1) dependencies[hash] |= 1U << 31;
                    }
                }
            Require(stage == p.environments.size(), "Pipeline stage/hash mismatch");
        }
    std::map<std::string, std::vector<std::vector<std::string>>> source, compute_source;
    std::vector<std::string> compute_order;
    JsonValue graphics = Array(), unresolved = Array();
    std::set<std::string> seen;
    for (const auto & p : model.pipelines)
    {
        bool located = true;
        for (const auto & id : p.environments)
            located &= !model.profile["shaders"][model.profile["environments"][id]["shader"].asString()]["references"].empty();
        if (!located)
        {
            Require(options.allow_partial, "Pipeline has unlocated shaders");
            continue;
        }
        const auto key = p.compute ? NormalizeCompute(p.key, dependencies) : Normalize(p.key, dependencies);
        auto & variants = (p.compute ? compute_source : source)[key];
        if (p.compute && variants.empty()) compute_order.push_back(key);
        if (std::find(variants.begin(), variants.end(), p.environments) == variants.end()) variants.push_back(p.environments);
    }
    std::set<std::string> emitted_compute;
    // Every environment variant recorded for a dispatched compute key, in first-use order.
    const auto emit_compute = [&](const std::string & key) {
        const auto found = compute_source.find(key);
        if (found == compute_source.end() || !emitted_compute.insert(key).second) return found != compute_source.end();
        for (const auto & environments : found->second)
        {
            Cursor fields{key};
            JsonValue r(JsonValueType::Object);
            r["kind"] = "compute";
            r["shader_hash"] = Num(fields.Get(8));
            r["shared_memory_size"] = Num(fields.Get(4));
            for (const auto field : {"workgroup_size", "cbuf_sizes"})
            {
                r[field] = Array();
                for (unsigned i = 0; i < (std::string_view(field) == "workgroup_size" ? 3U : 8U); ++i)
                    r[field].Append(Num(fields.Get(4)));
            }
            r["texture_table_size"] = Num(fields.Get(4));
            r["environments"] = Array();
            for (const auto & id : environments)
                r["environments"].Append(id);
            if (seen.insert(Canonical(r)).second)
            {
                graphics.Append(std::move(r));
                ++report.compute;
            }
        }
        return true;
    };
    Require(recording["recipes"].isArray() && recording["recipes"].size() <= MaxRecipes, "Guest observation limit");
    for (const auto & observation : recording["recipes"])
    {
        if (observation["kind"].asString() == "compute")
        {
            const auto & w = observation["words"];
            Require(w.isArray() && w.size() == Journal::ComputeWords, "Compute observation width");
            std::string key(Journal::ComputeWords * 4, '\0');
            for (unsigned i = 0; i < Journal::ComputeWords; ++i)
                Put(key, i * 4, Number(w[i]), 4);
            // A dispatch whose pipeline was never stored has no environment to share.
            if (!emit_compute(NormalizeCompute(std::move(key), dependencies))) ++report.unresolved;
            continue;
        }
        const auto source_bits = observation.isMember("source_feature_bits") ? Number(observation["source_feature_bits"]) : bits;
        Require(source_bits <= 127, "Unsupported source feature bits");
        const Vulkan::DynamicFeatures observation_features{bool(source_bits & 1), bool(source_bits & 2), bool(source_bits & 4), bool(source_bits & 8), bool(source_bits & 16), bool(source_bits & 32), bool(source_bits & 64)};
        const auto & w = observation["words"];
        Require(w.isArray() && w.size() == 382, "Guest observation width");
        std::vector<u32> words;
        words.reserve(382);
        for (const auto & n : w)
        {
            Require(n.Type() == JsonValueType::UnsignedInt || (n.Type() == JsonValueType::Int && n.asInt64() >= 0), "Guest word type");
            Require(n.asUInt64() <= UINT32_MAX, "Guest word overflow");
            words.push_back(static_cast<u32>(n.asUInt64()));
        }
        auto guest = Vulkan::GuestPipelineState::FromWords(std::span(words.data(), 278));
        Require(guest.has_value(), "Invalid guest state");
        const auto projected = guest->Project(observation_features);
        std::string key(1400, '\0');
        for (size_t i = 0; i < 104; ++i)
            Put(key, i * 4, words[278 + i], 4);
        std::memcpy(key.data() + 416, &projected, sizeof(projected));
        key = Normalize(std::move(key), dependencies);
        JsonValue r(JsonValueType::Object);
        r["kind"] = "graphics";
        r["guest_words"] = Array();
        for (size_t i = 0; i < 278; ++i)
            r["guest_words"].Append(words[i]);
        r["shader_hashes"] = Array();
        for (size_t i = 0; i < 6; ++i)
            r["shader_hashes"].Append(Num(u64{words[278 + 2 * i]} | (u64{words[279 + 2 * i]} << 32)));
        r["cbuf_sizes"] = Array();
        for (size_t stage = 0; stage < 5; ++stage)
        {
            JsonValue sizes = Array();
            for (size_t bank = 0; bank < 18; ++bank)
                sizes.Append(Num(At(key, 48 + (stage * 18 + bank) * 4, 4)));
            r["cbuf_sizes"].Append(std::move(sizes));
        }
        r["texture_pool"] = Array();
        r["texture_pool"].Append(Num(At(key, 408, 4)));
        r["texture_pool"].Append(Num(At(key, 412, 4)));
        auto found = source.find(key);
        bool hle_alias = false;
        if (found == source.end())
        {
            // Runtime already permits a plain draw to share a replacement-free
            // HLE pipeline. Recover the same directed alias at export, keeping
            // every other normalized byte (programs, sizes, pool and state).
            Vulkan::FixedPipelineState state{};
            state.raw2 = static_cast<u32>(At(key, 420, 4));
            using Hint = Tegra::Engines::Maxwell3D::EngineHint;
            if (state.app_stage == Hint::None)
            {
                state.app_stage.Assign(Hint::OnHLEMacro);
                auto twin = key;
                Put(twin, 420, state.raw2, 4);
                found = source.find(twin);
                hle_alias = found != source.end();
            }
        }
        bool matched = false;
        if (found != source.end())
        {
            for (auto it = found->second.rbegin(); it != found->second.rend(); ++it)
            {
                // Never drop real HLE replacements, nor infer that a plain
                // capture proves a future HLE draw has no replacements.
                if (hle_alias && std::any_of(it->begin(), it->end(), [&](const auto& id) {
                        return !model.profile["environments"][id]["cbuf_replacements"].empty();
                    }))
                    continue;
                matched = true;
                r["environments"] = Array();
                for (const auto & id : *it)
                    r["environments"].Append(id);
                if (seen.insert(Canonical(r)).second)
                {
                    graphics.Append(r);
                    ++report.graphics;
                }
            }
        }
        if (!matched)
        {
            r["reason"] = "no_serialized_environment_for_source_key";
            unresolved.Append(std::move(r));
            ++report.unresolved;
        }
        else if (hle_alias)
            ++report.resolved_hle_aliases;
    }
    // Journals written before dispatches were recorded: keep every local compute
    // pipeline of this version, after the recorded ones.
    if (!recording["records_compute"].asBool())
        for (const auto & key : compute_order)
            emit_compute(key);
    JsonValue result(JsonValueType::Object);
    result["format"] = "nxshader";
    result["version"] = 2U;
    result["guest_state_version"] = 1U;
    char title[17];
    std::snprintf(title, sizeof(title), "%016llx", static_cast<unsigned long long>(options.title_id));
    result["title_id"] = title;
    result["content_label"] = options.label;
    result["revision"] = options.revision;
    result["note"] = options.note;
    if (options.full_game) result["full_game"] = true;
    result["versions"] = JsonValue(JsonValueType::Object);
    if (const auto provenance = RecordedProvenance(options.cache.parent_path(), options.context, options.title_id); !provenance.isNull())
    {
        // Version numbers come from the game; one that is not plain short text is left out.
        const auto number = [](const JsonValue & value) {
            const auto text = value.isString() ? value.asString() : std::string{};
            return ValidProfileText(text, MaxVersionText) ? text : std::string{};
        };
        auto & version = result["versions"][options.context];
        version["base"] = number(provenance["base_version"]);
        version["update"] = provenance["update_enabled"].asBool() ? number(provenance["update_version"]) : std::string{};
    }
    result["source_files"] = model.profile["source_files"];
    result["source_context"] = options.context;
    result["shaders"] = JsonValue(JsonValueType::Object);
    result["environments"] = JsonValue(JsonValueType::Object);
    result["recipes"] = graphics;
    result["unresolved_recipes"] = unresolved;
    for (const auto & r : std::as_const(graphics))
        for (const auto & id : r["environments"])
        {
            const auto env = model.profile["environments"][id.asString()];
            result["environments"][id.asString()] = env;
            result["shaders"][env["shader"].asString()] = model.profile["shaders"][env["shader"].asString()];
        }
    return result;
}
// Programs take their emulator hash from the recipes that use them.
std::map<std::string, u64> ProgramHashes(const JsonValue & profile)
{
    std::map<std::string, u64> hashes;
    for (const auto & r : profile["recipes"])
    {
        const bool compute = r["kind"].asString() == "compute";
        size_t index{};
        for (unsigned slot = 0; slot < (compute ? 1U : 6U); ++slot)
        {
            const auto hash = compute ? Number(r["shader_hash"]) : Number(r["shader_hashes"][slot]);
            if (!hash) continue;
            Require(index < r["environments"].size(), "Recipe program count mismatch");
            const auto & env = profile["environments"][r["environments"][static_cast<unsigned>(index++)].asString()];
            hashes.emplace(env["shader"].asString(), hash);
        }
    }
    return hashes;
}
} // namespace

// Environment ids from their content only: sorted maps, shader keys remapped,
// and no source header digest, so one environment from two sources gets one id.
// Environments of a dropped program are removed with every recipe using them.
void CanonicalizeEnvironments(JsonValue & profile, const std::map<std::string, std::string> & shaders,
                              const std::set<std::string> & dropped)
{
    std::map<std::string, std::string> ids;
    JsonValue environments(JsonValueType::Object);
    for (const auto & id : profile["environments"].GetMemberNames())
    {
        JsonValue env = std::as_const(profile)["environments"][id];
        if (dropped.contains(env["shader"].asString())) continue;
        if (const auto found = shaders.find(env["shader"].asString()); found != shaders.end()) env["shader"] = found->second;
        env.removeMember("program_header_sha256");
        for (const auto name : MapNames)
        {
            std::vector<std::pair<u64, JsonValue>> entries;
            for (const auto & pair : std::as_const(env)[name])
                entries.emplace_back(Number(pair[0]), pair);
            std::stable_sort(entries.begin(), entries.end(), [](const auto & a, const auto & b) { return a.first < b.first; });
            JsonValue sorted = Array();
            for (auto & entry : entries)
                sorted.Append(std::move(entry.second));
            env[name] = std::move(sorted);
        }
        const auto canonical = ResourceSha256(Canonical(env));
        ids.emplace(id, canonical);
        environments[canonical] = std::move(env);
    }
    profile["environments"] = std::move(environments);
    JsonValue recipes = Array();
    for (const auto & r : std::as_const(profile)["recipes"])
    {
        JsonValue recipe = r;
        recipe["environments"] = Array();
        bool kept = true;
        for (const auto & id : r["environments"])
        {
            const auto found = ids.find(id.asString());
            kept &= found != ids.end();
            if (kept) recipe["environments"].Append(found->second);
        }
        if (kept) recipes.Append(std::move(recipe));
    }
    profile["recipes"] = std::move(recipes);
}
// The previous profile comes first, in its own order and with its own game
// versions, then what this capture adds. Nothing it carries needs local files:
// recipes of other versions keep their locators for the people who have them.
u64 MergeProfiles(JsonValue previous, JsonValue & current)
{
    u64 inherited{};
    // Game versions of the previous profile stay named; the new export's flag wins.
    if (current["versions"].isNull()) current["versions"] = JsonValue(JsonValueType::Object);
    if (previous["versions"].isObject())
        for (const auto & context : previous["versions"].GetMemberNames())
            if (!current["versions"].isMember(context)) current["versions"][context] = previous["versions"][context];
    // Every recipe lists its game versions; the capture's own default to its context.
    const auto context = current["source_context"].asString();
    for (auto * profile : {&previous, &current})
        for (const auto field : {"recipes", "unresolved_recipes"})
            for (unsigned i = 0; i < (*profile)[field].size(); ++i)
            {
                auto & r = (*profile)[field][i];
                if (!r["source_contexts"].isNull()) continue;
                const auto own = r["source_context"].isNull() ? (*profile)["source_context"].asString() : r["source_context"].asString();
                r.removeMember("source_context");
                r["source_contexts"] = Array();
                if (!own.empty()) r["source_contexts"].Append(own);
            }
    // One program, one key: a previous program this capture also located uses
    // the current entry and locator; a different program with its hash is dropped.
    std::map<u64, std::string> current_programs;
    for (const auto & [digest, hash] : ProgramHashes(current))
        current_programs.emplace(hash, digest);
    std::map<std::string, std::string> shaders;
    std::set<std::string> dropped;
    for (const auto & [digest, hash] : ProgramHashes(previous))
        if (const auto found = current_programs.find(hash); found != current_programs.end())
        {
            if (Number(std::as_const(current)["shaders"][found->second]["size"]) == Number(std::as_const(previous)["shaders"][digest]["size"]))
                shaders.emplace(digest, found->second);
            else
                dropped.insert(digest);
        }
    // Source files are matched by size and header at import; a name that the
    // current files already use for other content is renamed.
    std::map<std::string, std::string> renamed;
    {
        std::map<std::string, std::pair<u64, std::string>> files;
        if (current["source_files"].isNull()) current["source_files"] = Array();
        for (const auto & file : std::as_const(current)["source_files"])
            files.emplace(file["name"].asString(), std::pair{Number(file["size"]), file["header"].asString()});
        for (const auto & file : std::as_const(previous)["source_files"])
        {
            const auto identity = std::pair{Number(file["size"]), file["header"].asString()};
            auto name = file["name"].asString();
            if (const auto found = files.find(name); found != files.end())
            {
                if (found->second == identity) continue;
                for (unsigned n = 1; files.contains(name); ++n)
                    name = file["name"].asString() + "~" + std::to_string(n);
                renamed.emplace(file["name"].asString(), name);
            }
            files.emplace(name, identity);
            JsonValue entry = file;
            entry["name"] = name;
            current["source_files"].Append(std::move(entry));
        }
    }
    const auto rename = [&](JsonValue & locator) {
        if (locator.isMember("file") && renamed.contains(locator["file"].asString()))
            locator["file"] = renamed.at(locator["file"].asString());
        if (!locator.isMember("physical_spans")) return;
        for (unsigned n = 0; n < locator["physical_spans"].size(); ++n)
        {
            auto & span = locator["physical_spans"][n];
            if (renamed.contains(span["file"].asString())) span["file"] = renamed.at(span["file"].asString());
        }
    };
    for (const auto & digest : previous["shaders"].GetMemberNames())
    {
        if (dropped.contains(digest)) continue;
        JsonValue shader = std::as_const(previous)["shaders"][digest];
        for (unsigned i = 0; i < shader["references"].size(); ++i)
            rename(shader["references"][i]);
        const auto found = shaders.find(digest);
        if (found == shaders.end())
        {
            current["shaders"][digest] = std::move(shader);
            continue;
        }
        // A program both versions contain keeps each version's locator, so a
        // player of either version finds it in the files they have.
        auto & references = current["shaders"][found->second]["references"];
        std::set<std::string> known;
        for (const auto & locator : std::as_const(references))
            known.insert(Canonical(locator));
        for (const auto & locator : std::as_const(shader)["references"])
            if (references.size() < MaxProgramLocators && known.insert(Canonical(locator)).second)
                references.Append(locator);
    }
    CanonicalizeEnvironments(previous, shaders, dropped);
    for (const auto & id : previous["environments"].GetMemberNames())
        current["environments"][id] = std::as_const(previous)["environments"][id];
    // A recipe both profiles hold, for whichever versions, is kept once at its
    // first position with the versions of both: the previous profile's first.
    const auto content = [](JsonValue r) {
        r.removeMember("source_contexts");
        return Canonical(r);
    };
    const auto append = [](JsonValue & table, std::map<std::string, unsigned> & index, const std::string & key, const JsonValue & r) {
        const auto [found, inserted] = index.emplace(key, table.size());
        if (inserted)
        {
            table.Append(r);
            return;
        }
        auto & versions = table[found->second]["source_contexts"];
        std::set<std::string> known;
        for (const auto & version : std::as_const(versions))
            known.insert(version.asString());
        for (const auto & version : r["source_contexts"])
            if (known.insert(version.asString()).second) versions.Append(version);
    };
    std::set<std::string> captured;
    for (const auto & r : std::as_const(current)["recipes"])
        captured.insert(content(r));
    JsonValue recipes = Array();
    std::map<std::string, unsigned> recipe_index;
    for (const auto * table : {&previous, &current})
        for (const auto & r : std::as_const(*table)["recipes"])
        {
            const auto key = content(r);
            inherited += table == &previous && !captured.contains(key) && !recipe_index.contains(key);
            append(recipes, recipe_index, key, r);
        }
    current["recipes"] = std::move(recipes);
    // An observation stays unresolved only while no recipe resolves it.
    const auto observation = [](const JsonValue & r) {
        JsonValue key(JsonValueType::Object);
        for (const auto field : {"guest_words", "shader_hashes", "cbuf_sizes", "texture_pool"})
            key[field] = r[field];
        return Canonical(key);
    };
    std::set<std::string> resolved;
    for (const auto & r : std::as_const(current)["recipes"])
        if (r["kind"].asString() == "graphics") resolved.insert(observation(r));
    JsonValue unresolved = Array();
    std::map<std::string, unsigned> unresolved_index;
    for (const auto * table : {&previous, &current})
        for (const auto & r : std::as_const(*table)["unresolved_recipes"])
            if (const auto key = observation(r); !resolved.contains(key)) append(unresolved, unresolved_index, key, r);
    current["unresolved_recipes"] = std::move(unresolved);
    return inherited;
}

TrimReport TrimToLimits(JsonValue & profile, const std::string & keep, const ProfileLimits & limits)
{
    TrimReport report;
    const auto versions = [](const JsonValue & r) {
        std::vector<std::string> result;
        for (const auto & version : r["source_contexts"])
            result.push_back(version.asString());
        if (r["source_contexts"].isNull() && r["source_context"].isString()) result.push_back(r["source_context"].asString());
        return result;
    };
    // Oldest first: a merge puts the installed profile's recipes before the new ones.
    std::vector<std::string> order;
    for (const auto field : {"recipes", "unresolved_recipes"})
        for (const auto & r : std::as_const(profile)[field])
            for (const auto & version : versions(r))
                if (version != keep && std::find(order.begin(), order.end(), version) == order.end())
                    order.push_back(version);
    for (size_t next = 0;; ++next)
    {
        // Keep only what the remaining recipes use.
        std::set<std::string> environments, programs;
        for (const auto & r : std::as_const(profile)["recipes"])
            for (const auto & id : r["environments"])
                environments.insert(id.asString());
        size_t map_entries{};
        for (const auto & id : environments)
        {
            const auto & env = std::as_const(profile)["environments"][id];
            programs.insert(env["shader"].asString());
            for (const auto name : MapNames)
                map_entries += env[name].size();
        }
        u64 code_bytes{};
        for (const auto & digest : programs)
            code_bytes += Number(std::as_const(profile)["shaders"][digest]["size"]);
        for (const auto field : {"environments", "shaders"})
        {
            const auto & used = std::string_view(field) == "shaders" ? programs : environments;
            for (const auto & key : profile[field].GetMemberNames())
                if (!used.contains(key)) profile[field].removeMember(key.c_str());
        }
        const bool fits = profile["recipes"].size() <= limits.recipes && programs.size() <= limits.programs &&
                          environments.size() <= limits.environments && map_entries <= limits.map_entries &&
                          code_bytes <= limits.code_bytes;
        if (fits || next == order.size()) break;
        const auto & version = order[next];
        ++report.versions;
        for (const auto field : {"recipes", "unresolved_recipes"})
        {
            JsonValue kept = Array();
            for (const auto & r : std::as_const(profile)[field])
            {
                auto remaining = versions(r);
                const auto found = std::find(remaining.begin(), remaining.end(), version);
                if (found == remaining.end())
                {
                    kept.Append(r);
                    continue;
                }
                remaining.erase(found);
                if (remaining.empty())
                {
                    report.recipes += std::string_view(field) == "recipes";
                    continue;
                }
                JsonValue copy = r;
                copy.removeMember("source_context");
                copy["source_contexts"] = Array();
                for (const auto & other : remaining)
                    copy["source_contexts"].Append(other);
                kept.Append(std::move(copy));
            }
            profile[field] = std::move(kept);
        }
    }
    // Observations without a recipe are informative only: keep the newest.
    if (profile["unresolved_recipes"].size() > limits.unresolved)
    {
        JsonValue kept = Array();
        for (auto i = profile["unresolved_recipes"].size() - static_cast<unsigned>(limits.unresolved); i < profile["unresolved_recipes"].size(); ++i)
            kept.Append(std::as_const(profile)["unresolved_recipes"][i]);
        profile["unresolved_recipes"] = std::move(kept);
    }
    return report;
}

std::string Canonical(const JsonValue & value)
{
    const auto styled = JsonStyledWriter().write(value);
    std::string result;
    result.reserve(styled.size());
    bool quoted = false, escaped = false;
    for (char c : styled)
    {
        if (quoted)
        {
            result += c;
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        }
        else if (c == '"')
        {
            quoted = true;
            result += c;
        }
        else if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            result += c;
    }
    return result;
}
JsonValue ParseMetadata(std::string_view text, size_t limit)
{
    Require(text.size() <= limit, "Metadata size limit");
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (char c : text)
    {
        if (quoted)
        {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        }
        else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[')
            Require(++depth <= 32, "Metadata depth limit");
        else if (c == '}' || c == ']')
        {
            Require(depth > 0, "Metadata structure");
            --depth;
        }
    }
    Require(!quoted && depth == 0, "Incomplete metadata");
    JsonValue value;
    Require(JsonReader().Parse(text.data(), text.data() + text.size(), value), "Invalid metadata JSON");
    return value;
}
void Publish(const std::filesystem::path & output, std::string_view bytes)
{
    Require(!std::filesystem::exists(output), "Output already exists");
    auto temporary = output;
    temporary += ".partial";
    Require(!std::filesystem::exists(temporary), "Output temporary already exists");
    try
    {
        std::ofstream out(temporary, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        Require(bool(out), "Cannot write profile");
        out.close();
        Require(!std::filesystem::exists(output), "Output already exists");
#ifdef _WIN32
        Require(MoveFileW(temporary.c_str(), output.c_str()) != 0, "Cannot publish profile (output may already exist)");
#else
        Require(link(temporary.c_str(), output.c_str()) == 0, "Cannot publish profile (output may already exist)");
        std::filesystem::remove(temporary);
#endif
    }
    catch (...)
    {
        std::error_code error;
        std::filesystem::remove(temporary, error);
        throw;
    }
}
ExportReport Export(const ExportOptions & requested)
{
    const auto started = std::chrono::steady_clock::now();
    auto options = requested;
    const auto progress = [&](const char * phase, u64 done, u64 total) {
        Require(!options.progress || options.progress(phase, done, total), "Shader profile operation cancelled");
    };
    progress("identify-content", 0, 0);
    if (options.automatic_sources) {
        ResolveRecordedSources(options);
        // The caller's label (game name and version) wins; the recorded version is the fallback.
        if (options.label.empty() || !ValidProfileText(options.label, 4096))
            options.label = RecordedVersionLabel(RecordedProvenance(options.cache.parent_path(), options.context, options.title_id));
    }
    Require(options.title_id && !options.base.empty(), "Missing game identity");
    Require(ValidProfileText(options.revision, MaxRevision) && ValidProfileText(options.note, MaxNote),
            "The version or note is too long or contains control characters");
    Require(!std::filesystem::exists(options.output), "Output already exists");
    progress("read-cache", 0, 0);
    auto model = ReadCache(options);
    ExportReport report;
    Discover(options, model.profile, model.codes, report);
    progress("assemble", 0, 0);
    auto profile = Assemble(model, options, report);
    CanonicalizeEnvironments(profile);
    if (!options.previous.empty())
    {
        // A damaged or foreign installed profile is reported, never fatal: the
        // runtime rejects it as well, so none of its recipes are in use.
        try
        {
            const auto packed = Load(options.previous, MaxProfileFileBytes);
            Require(packed.starts_with("NXSHADR2"), "Expected a compressed NXShader profile");
            report.inherited = MergeProfiles(DecodeCompact(Unpack(packed), options.title_id), profile);
            report.previous = ExportReport::Previous::Merged;
        }
        catch (const std::exception &)
        {
            report.previous = ExportReport::Previous::Unreadable;
        }
    }
    {
        const auto trimmed = TrimToLimits(profile, options.context, ProfileLimits{});
        report.dropped_recipes = trimmed.recipes;
        report.dropped_versions = trimmed.versions;
    }
    Require(!profile["recipes"].empty(), "No reconstructible recipes");
    progress("compress", 0, 0);
    const auto bytes = PackCompact(profile, options.compression_level);
    {
        // Counts of what was written, after merging and deduplication.
        const auto written = InspectCompact(Unpack(bytes), options.title_id);
        report.graphics = written.graphics;
        report.compute = written.compute;
        report.unresolved = written.unresolved;
        report.programs = written.programs;
        report.environments = written.environments;
        report.versions = written.versions;
    }
    progress("write-profile", 0, bytes.size());
    Publish(options.output, bytes);
    report.bytes = bytes.size();
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return report;
}
} // namespace VideoCommon::ShaderProfile
