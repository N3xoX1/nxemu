// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_compact.h"
#include "yuzu_video_core/shader_profile_codec.h"
#include "yuzu_video_core/shader_profile_container.h"
#include "yuzu_video_core/shader_profile_export.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_metrics.h"
#include "yuzu_video_core/shader_profile_resources.h"
#include "yuzu_video_core/shader_profile_validation.h"
#include "yuzu_video_core/shader_profile_context.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <exception>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <system_error>
#include <thread>
#include <set>
#include <tuple>
#include <utility>

namespace VideoCommon::ShaderProfile
{
namespace
{
void Check(bool b, const char * reason)
{
    if (!b) throw std::runtime_error(reason);
}
void Allowed(const JsonValue & value, std::initializer_list<const char *> fields)
{
    Check(value.isObject(), "Expected compact metadata object");
    for (const auto & field : value.GetMemberNames())
        Check(std::find(fields.begin(), fields.end(), field) != fields.end(), "Unknown compact metadata field");
}
std::string Hash(std::string_view bytes, Timing::Metrics * timings)
{
    return Timing::Measure(timings, Timing::Phase::ResourceVerification, [&] { return ResourceSha256(bytes); });
}
bool Verify(std::string_view program, u64 hash, Timing::Metrics * timings)
{
    return Timing::Measure(timings, Timing::Phase::ResourceVerification, [&] { return MatchesProgramHash(program, hash); });
}
u64 Number(const JsonValue & n)
{
    if (n.isString())
    {
        const auto s = n.asString();
        Check(!s.empty() && s.size() <= 20 && s.find_first_not_of("0123456789") == std::string::npos, "Invalid compact integer");
        return std::stoull(s);
    }
    Check(n.Type() == JsonValueType::UnsignedInt || (n.Type() == JsonValueType::Int && n.asInt64() >= 0), "Expected compact integer");
    return n.asUInt64();
}
struct Writer
{
    std::string bytes;
    void Fixed(u64 n, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
            bytes += static_cast<char>(n >> (i * 8));
    }
    void Var(u64 n)
    {
        while (n >= 128)
        {
            bytes += static_cast<char>((n & 127) | 128);
            n >>= 7;
        }
        bytes += static_cast<char>(n);
    }
    void Text(std::string_view s)
    {
        Var(s.size());
        bytes += s;
    }
    void Hash(const std::string & h)
    {
        Check(h.size() == 64 && h.find_first_not_of("0123456789abcdef") == std::string::npos, "Invalid compact SHA-256");
        for (size_t i = 0; i < 64; i += 2)
        {
            const auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            bytes += static_cast<char>((hex(h[i]) << 4) | hex(h[i + 1]));
        }
    }
};
struct Reader
{
    std::string_view data;
    size_t pos{};
    std::string_view Take(size_t n)
    {
        Check(pos <= data.size() && n <= data.size() - pos, "Truncated compact table");
        const auto result = data.substr(pos, n);
        pos += n;
        return result;
    }
    u64 Fixed(size_t n)
    {
        const auto b = Take(n);
        u64 v{};
        for (size_t i = 0; i < n; ++i)
            v |= u64{static_cast<unsigned char>(b[i])} << (i * 8);
        return v;
    }
    u64 Var()
    {
        u64 value{};
        for (unsigned i = 0; i < 10; ++i)
        {
            const auto b = Fixed(1);
            Check(i < 9 || b <= 1, "Compact varint overflow");
            value |= (b & 127) << (7 * i);
            if (b < 128)
            {
                Check(i == 0 || b != 0, "Noncanonical compact varint");
                return value;
            }
        }
        throw std::runtime_error("Compact varint overflow");
    }
    u32 Word()
    {
        const auto n = Var();
        Check(n <= UINT32_MAX, "Compact word overflow");
        return static_cast<u32>(n);
    }
    size_t Count(size_t maximum)
    {
        const auto n = Var();
        Check(n <= maximum && n <= data.size() - pos, "Compact table limit");
        return static_cast<size_t>(n);
    }
    std::string Text(size_t maximum = 4096)
    {
        const auto n = Count(maximum);
        return std::string(Take(n));
    }
    std::string Hash()
    {
        const auto b = Take(32);
        constexpr char hex[] = "0123456789abcdef";
        std::string h;
        h.reserve(64);
        for (unsigned char c : b)
        {
            h += hex[c >> 4];
            h += hex[c & 15];
        }
        return h;
    }
};
const std::array<const char *, 7> Fields{"local_memory_size", "texture_bound", "guest_start_address", "guest_read_lowest", "guest_read_highest", "viewport_transform_state", "stage"};
// Not stored: both follow from the start address and the program size.
constexpr size_t StartAddress = 2, ReadLowest = 3, ReadHighest = 4;
const std::array<const char *, 4> Maps{"texture_types", "texture_pixel_formats", "cbuf_values", "cbuf_replacements"};
const std::array<const char *, 5> Encodings{"raw", "yaz0", "nso-lz4", "bktr-raw", "bktr-yaz0"};
struct Span
{
    std::string file;
    u64 offset{}, size{};
};
struct Resource
{
    unsigned kind{};
    std::string file, hash, segment_hash;
    u64 offset{}, size{}, decoded_size{};
    std::vector<Span> spans;
};
struct Locator
{
    u32 resource{};
    u64 offset{};
};
struct Program
{
    u64 hash{}; // Emulator program hash; recipes derive their shader hashes from it.
    u64 size{};
    std::vector<Locator> locators;
};
constexpr u64 NoSlot = ~u64{};
// Recipe slot of a graphics environment stage: VertexA (6) first, then stages 0-4.
u64 GraphicsSlot(u64 stage)
{
    return stage == 6 ? 0 : stage < 5 ? stage + 1 : NoSlot;
}
struct Env
{
    u32 program{};
    std::array<u32, 7> fields{};
    std::array<std::vector<std::pair<u64, u32>>, 4> maps;
    std::array<u32, 3> workgroup{};
    u32 shared{};
    std::array<u32, 8> passthrough{};
};
// Where one NCA named by a profile is, in the game containers beside the game.
struct ContentLocation
{
    std::filesystem::path path;
    u64 offset{}, size{};
};
using ContentMap = std::map<std::string, ContentLocation>;
const ContentLocation & Locate(const ContentMap & contents, const std::string & name)
{
    const auto found = contents.find(name);
    if (found == contents.end())
        throw std::filesystem::filesystem_error("Game content not found", std::filesystem::u8path(name),
                                                std::make_error_code(std::errc::no_such_file_or_directory));
    return found->second;
}
std::string Bytes(const ContentMap & contents, const std::string & name, u64 off, u64 size, Timing::Metrics * timings)
{
    Timing::Timer timer(timings, Timing::Phase::ResourceIO);
    const auto & content = Locate(contents, name);
    Check(size <= MaxDecodedResource && off <= content.size && size <= content.size - off, "Compact resource range");
    std::string bytes(static_cast<size_t>(size), '\0');
    std::ifstream in(content.path, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(content.offset + off));
    in.read(bytes.data(), size);
    Check(bool(in), "Cannot read compact resource");
    if (timings) timings->Add(Timing::Counter::ResourceReadBytes, size);
    return bytes;
}
SharedShaderBytes Environment(const Env & env, std::shared_ptr<const std::string> program, Timing::Metrics * timings)
{
    const std::string_view code=*program;
    Check(env.fields[6] <= 6 && env.fields[2] == env.fields[3] && env.fields[4] >= env.fields[3] && u64{env.fields[4]} - env.fields[3] + 8 == code.size(), "Compact environment bounds");
    Writer out;
    out.Fixed(code.size(), 8);
    for (const auto & m : env.maps)
        out.Fixed(m.size(), 8);
    for (auto n : env.fields)
        out.Fixed(n, 4);
    for (size_t i = 0; i < 4; ++i)
        for (const auto & [key, value] : env.maps[i])
        {
            out.Fixed(key, i < 2 ? 4 : 8);
            out.Fixed(value, 4);
        }
    if (env.fields[6] == 5)
    {
        for (auto n : env.workgroup)
            out.Fixed(n, 4);
        out.Fixed(env.shared, 4);
    }
    else
    {
        Check(code.size() >= 80, "Compact graphics header size");
        out.bytes += code.substr(0, 80);
        if (env.fields[6] == 3)
            for (auto n : env.passthrough)
                out.Fixed(n, 4);
    }
    return SharedShaderBytes::FromMetadata(std::move(out.bytes),std::move(program));
}
} // namespace

std::string PackCompact(const JsonValue & profile, int compression_level)
{
    Allowed(profile, {"format", "version", "title_id", "guest_state_version", "content_label", "revision", "note", "full_game", "versions", "shaders", "environments", "recipes", "unresolved_recipes", "source_files", "source_context"});
    Check(profile["shaders"].isObject() && profile["shaders"].size() <= ProfileLimits{}.programs && profile["environments"].isObject() && profile["environments"].size() <= ProfileLimits{}.environments && profile["recipes"].isArray() && profile["recipes"].size() <= ProfileLimits{}.recipes && (profile["unresolved_recipes"].isNull() || (profile["unresolved_recipes"].isArray() && profile["unresolved_recipes"].size() <= ProfileLimits{}.unresolved)), "Compact table limit");
    const auto title = profile["title_id"].asString();
    Check(title.size() == 16 && title.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos, "Compact title syntax");
    const auto text = [&](const char* field) {
        Check(profile[field].isNull() || profile[field].isString(), "Invalid profile description");
        return profile[field].isNull() ? std::string{} : profile[field].asString();
    };
    const auto label = text("content_label"), revision = text("revision"), note = text("note");
    Check(ValidProfileText(label, 4096) && ValidProfileText(revision, MaxRevision) && ValidProfileText(note, MaxNote),
          "Invalid profile description");
    Check(Number(profile["guest_state_version"]) == 1, "Compact guest state version");
    Check(profile["version"].asUInt64() == 2 && profile["format"].asString() == "nxshader", "Compact encoder requires v2 metadata");
    Writer out;
    out.bytes = "NXSHBIN1";
    out.Fixed(std::stoull(profile["title_id"].asString(), nullptr, 16), 8);
    out.Text(label);
    out.Text(revision);
    out.Text(note);
    // Flags the author sets; bit 0: the recordings cover the whole game.
    Check(profile["full_game"].isNull() || profile["full_game"].isBool(), "Invalid profile flags");
    out.Var(profile["full_game"].asBool() ? 1 : 0);
    // Resources name the game contents (NCAs) listed here.
    std::set<std::string> source_names;
    {
        Check((profile["source_files"].isNull() || profile["source_files"].isArray()) && profile["source_files"].size() <= 1024, "Invalid source file table");
        out.Var(profile["source_files"].size());
        for (const auto& file : profile["source_files"]) {
            Allowed(file, {"name", "size", "header"});
            source_names.insert(file["name"].asString());
            out.Text(file["name"].asString()); out.Var(Number(file["size"])); out.Hash(file["header"].asString());
        }
    }
    // Contexts are stored once; recipes refer to them by index + 1, 0 meaning none.
    // A recipe recorded with several game versions lists each of them.
    const auto recipe_contexts = [&](const JsonValue & r) {
        std::vector<std::string> result;
        if (!r["source_contexts"].isNull())
        {
            Check(r["source_context"].isNull() && r["source_contexts"].isArray(), "Invalid recipe contexts");
            for (const auto & context : r["source_contexts"])
                if (std::find(result.begin(), result.end(), context.asString()) == result.end())
                    result.push_back(context.asString());
        }
        else if (const auto context = r["source_context"].isNull() ? profile["source_context"].asString() : r["source_context"].asString(); !context.empty())
            result.push_back(context);
        Check(std::find(result.begin(), result.end(), std::string{}) == result.end(), "Invalid recipe contexts");
        return result;
    };
    std::map<std::string, size_t> context_ids;
    std::vector<std::string> contexts;
    const auto intern = [&](const std::string & context) {
        if (!context.empty() && context_ids.emplace(context, contexts.size() + 1).second) contexts.push_back(context);
    };
    intern(profile["source_context"].asString());
    for (const auto field : {"recipes", "unresolved_recipes"})
        for (const auto & r : profile[field])
            for (const auto & context : recipe_contexts(r))
                intern(context);
    Check(contexts.size() <= 65536, "Compact context limit");
    out.Var(contexts.size());
    for (const auto & context : contexts)
        out.Hash(context);
    // The game version each context was recorded with: base and update version
    // numbers, for people only. Recipes are still verified against the local game.
    Check(profile["versions"].isNull() || profile["versions"].isObject(), "Invalid profile versions");
    for (const auto & context : contexts)
    {
        const auto & version = profile["versions"][context];
        const auto number = [&](const char * field) {
            const auto value = version[field].isString() ? version[field].asString() : std::string{};
            Check(ValidProfileText(value, MaxVersionText), "Invalid profile version");
            return value;
        };
        out.Text(number("base"));
        out.Text(number("update"));
    }
    const auto context_index = [&](const std::string & context) { return context.empty() ? 0 : context_ids.at(context); };
    out.Var(context_index(profile["source_context"].asString()));
    // Each program takes its hash from the recipes that use it, so recipes store
    // none and only programs and environments reachable from a recipe are kept.
    std::map<std::string, u64> program_hashes;
    std::set<std::string> used_environments;
    for (const auto & r : profile["recipes"])
    {
        Check(r["environments"].isArray(), "Invalid recipe environments");
        const bool compute = r["kind"].asString() == "compute";
        std::vector<std::pair<u64, u64>> active; // slot, hash
        if (compute)
            active.emplace_back(0, Number(r["shader_hash"]));
        else
        {
            Check(r["shader_hashes"].isArray() && r["shader_hashes"].size() == 6, "Invalid graphics recipe arrays");
            for (u64 slot = 0; slot < 6; ++slot)
                if (const auto hash = Number(r["shader_hashes"][static_cast<unsigned>(slot)])) active.emplace_back(slot, hash);
        }
        Check(!active.empty() && active.size() == r["environments"].size(), "Recipe program count mismatch");
        for (size_t i = 0; i < active.size(); ++i)
        {
            const auto id = r["environments"][static_cast<unsigned>(i)].asString();
            const auto & env = profile["environments"][id];
            Check(env.isObject(), "Unknown recipe environment");
            const auto stage = Number(env["stage"]);
            Check(active[i].second != 0 && (compute ? stage == 5 : GraphicsSlot(stage) == active[i].first), "Recipe stage order mismatch");
            const auto [found, inserted] = program_hashes.emplace(env["shader"].asString(), active[i].second);
            Check(found->second == active[i].second, "Ambiguous program hash");
            used_environments.insert(id);
        }
    }
    {
        std::set<u64> distinct;
        for (const auto & [digest, hash] : program_hashes)
            Check(distinct.insert(hash).second, "Two programs share one hash");
    }
    std::vector<JsonValue> resources;
    std::map<std::string, size_t> resource_ids, program_ids, env_ids, state_ids;
    struct Ref
    {
        u64 size, hash;
        std::vector<std::pair<size_t, u64>> locators; // resource, offset
    };
    std::vector<Ref> programs;
    std::vector<std::string> shader_names;
    for (const auto & [digest, hash] : program_hashes)
    {
        Check(profile["shaders"][digest].isObject(), "Unknown recipe program");
        shader_names.push_back(digest);
    }
    std::map<std::string,std::string> locator_keys;
    for (const auto& name : shader_names) locator_keys.emplace(name,Canonical(profile["shaders"][name]["references"]));
    std::sort(shader_names.begin(), shader_names.end(), [&](const auto & a, const auto & b) {
        return std::tie(locator_keys.at(a),a) < std::tie(locator_keys.at(b),b);
    });
    for (const auto & h : shader_names)
    {
        const auto & shader = profile["shaders"][h];
        Allowed(shader, {"size", "references"});
        Check(shader["references"].isArray() && shader["references"].size() >= 1 &&
              shader["references"].size() <= MaxProgramLocators, "Compact program locator count");
        Ref ref{Number(shader["size"]), program_hashes.at(h), {}};
        for (unsigned i = 0; i < shader["references"].size(); ++i)
        {
            auto descriptor = shader["references"][i];
            Allowed(descriptor, {"encoding", "file", "offset", "size", "container_offset", "container_size", "decoded_size", "container_sha256", "segment_sha256", "physical_spans", "resource_size", "resource_sha256"});
            const auto offset = Number(descriptor["offset"]);
            Check(Number(descriptor["size"]) == ref.size, "Compact locator size");
            descriptor.removeMember("offset");
            descriptor.removeMember("size");
            const auto key = Canonical(descriptor);
            auto [it, inserted] = resource_ids.emplace(key, resources.size());
            if (inserted) resources.push_back(std::move(descriptor));
            const std::pair locator{it->second, offset};
            Check(std::find(ref.locators.begin(), ref.locators.end(), locator) == ref.locators.end(), "Duplicate compact locator");
            ref.locators.push_back(locator);
        }
        program_ids[h] = programs.size();
        programs.push_back(std::move(ref));
    }
    out.Var(resources.size());
    for (const auto & resource : resources)
    {
        const auto encoding = resource["encoding"].asString();
        const auto it = std::find(Encodings.begin(), Encodings.end(), encoding);
        Check(it != Encodings.end(), "Unknown compact resource encoding");
        const auto kind = it - Encodings.begin();
        out.Var(kind);
        Check(kind >= 3 || source_names.contains(resource["file"].asString()), "Resource outside the game contents");
        for (const auto & span : resource["physical_spans"])
            Check(source_names.contains(span["file"].asString()), "Resource outside the game contents");
        if (kind < 3) out.Text(resource["file"].asString());
        if (kind == 1 || kind == 2)
        {
            out.Var(Number(resource["container_offset"]));
            out.Var(Number(resource["container_size"]));
        }
        if (kind == 2)
        {
            out.Var(Number(resource["decoded_size"]));
            out.Hash(resource["container_sha256"].asString());
            out.Hash(resource["segment_sha256"].asString());
        }
        if (kind >= 3)
        {
            out.Var(Number(resource["resource_size"]));
            out.Hash(resource["resource_sha256"].asString());
            out.Var(resource["physical_spans"].size());
            for (const auto & span : resource["physical_spans"])
            {
                out.Text(span["file"].asString());
                out.Var(Number(span["offset"]));
                out.Var(Number(span["size"]));
            }
        }
    }
    out.Var(programs.size());
    for (const auto & p : programs)
    {
        out.Fixed(p.hash, 8);
        out.Var(p.size);
        out.Var(p.locators.size());
        for (const auto & [resource, offset] : p.locators)
        {
            out.Var(resource);
            out.Var(offset);
        }
    }
    // Environments are stored in a canonical encoding (sorted maps) and identical
    // encodings once, whatever their metadata ids, so merged profiles stay minimal.
    std::vector<std::pair<std::string, std::string>> encoded; // encoding, id
    for (const auto & id : used_environments)
    {
        const auto & env = profile["environments"][id];
        Allowed(env, {"shader", "local_memory_size", "texture_bound", "guest_start_address", "guest_read_lowest", "guest_read_highest", "viewport_transform_state", "stage", "texture_types", "texture_pixel_formats", "cbuf_values", "cbuf_replacements", "workgroup_size", "shared_memory_size", "program_header_sha256", "gp_passthrough_mask"});
        Writer e;
        const auto program = program_ids.at(env["shader"].asString());
        e.Var(program);
        // The read range always starts at the program and covers it, which the
        // reader requires; only the start address is stored.
        const auto start = Number(env["guest_start_address"]);
        Check(Number(env["guest_read_lowest"]) == start && programs[program].size >= 8 &&
              Number(env["guest_read_highest"]) == start + programs[program].size - 8, "Compact environment bounds");
        for (size_t field = 0; field < Fields.size(); ++field)
            if (field != ReadLowest && field != ReadHighest) e.Var(Number(env[Fields[field]]));
        for (const auto name : Maps)
        {
            Check(env[name].isArray(), "Invalid environment map");
            std::vector<std::pair<u64, u64>> entries;
            for (const auto & pair : env[name])
            {
                Check(pair.isArray() && pair.size() == 2, "Invalid environment map entry");
                entries.emplace_back(Number(pair[0]), Number(pair[1]));
            }
            std::sort(entries.begin(), entries.end());
            Check(std::adjacent_find(entries.begin(), entries.end(), [](const auto & a, const auto & b) { return a.first == b.first; }) == entries.end(),
                  "Duplicate environment map key");
            // Keys are sorted and distinct: each is stored as the gap after the previous one.
            e.Var(entries.size());
            for (size_t n = 0; n < entries.size(); ++n)
            {
                e.Var(n ? entries[n].first - entries[n - 1].first - 1 : entries[n].first);
                e.Var(entries[n].second);
            }
        }
        const auto stage = Number(env["stage"]);
        if (stage == 5)
        {
            Check(env["workgroup_size"].isArray() && env["workgroup_size"].size() == 3, "Invalid compute dimensions");
            for (const auto & n : env["workgroup_size"])
                e.Var(Number(n));
            e.Var(Number(env["shared_memory_size"]));
        }
        else
        {
            // The program's own bytes authenticate its header; a source digest is optional.
            if (env.isMember("program_header_sha256"))
            {
                const auto header = env["program_header_sha256"].asString();
                Check(header.size() == 64 && header.find_first_not_of("0123456789abcdef") == std::string::npos,
                      "Invalid compact header SHA-256");
            }
            if (stage == 3)
            {
                Check(env["gp_passthrough_mask"].isArray() && env["gp_passthrough_mask"].size() == 8, "Invalid geometry mask");
                for (const auto & n : env["gp_passthrough_mask"])
                    e.Var(Number(n));
            }
        }
        encoded.emplace_back(std::move(e.bytes), id);
    }
    // Program order first keeps related environments adjacent for compression.
    std::sort(encoded.begin(), encoded.end(), [&](const auto & a, const auto & b) {
        const auto program = [&](const std::string & id) { return program_ids.at(profile["environments"][id]["shader"].asString()); };
        return std::forward_as_tuple(program(a.second), a.first, a.second) < std::forward_as_tuple(program(b.second), b.first, b.second);
    });
    {
        std::map<std::string, size_t> encodings;
        std::string table;
        for (const auto & [bytes, id] : encoded)
        {
            const auto [found, inserted] = encodings.emplace(bytes, encodings.size());
            if (inserted) table += bytes;
            env_ids[id] = found->second;
        }
        out.Var(encodings.size());
        out.bytes += table;
    }
    // Guest states are deduplicated independently of program combinations.
    std::vector<std::vector<u32>> states;
    for (const auto field : {"recipes", "unresolved_recipes"})
        for (const auto & recipe : profile[field])
            if (recipe["kind"].asString() == "graphics")
            {
                const auto key = Canonical(recipe["guest_words"]);
                if (!state_ids.contains(key))
                {
                    std::vector<u32> w;
                    for (const auto & n : recipe["guest_words"])
                    {
                        Check(Number(n) <= UINT32_MAX, "State word overflow");
                        w.push_back(static_cast<u32>(Number(n)));
                    }
                    Check(w.size() == 278, "Compact guest state ABI");
                    state_ids[key] = states.size();
                    states.push_back(std::move(w));
                }
            }
    std::array<u32, 278> base{};
    for (size_t word = 0; word < 278; ++word)
    {
        std::map<u32, size_t> counts;
        size_t best{};
        for (const auto & state : states)
            if (const auto count = ++counts[state[word]]; count > best)
            {
                best = count;
                base[word] = state[word];
            }
    }
    for (auto n : base)
        out.Var(n);
    out.Var(states.size());
    for (const auto & state : states)
    {
        size_t count{};
        for (size_t i = 0; i < 278; ++i)
            count += state[i] != base[i];
        out.Var(count);
        for (size_t i = 0; i < 278; ++i)
            if (state[i] != base[i])
            {
                out.Var(i);
                out.Var(state[i]);
            }
    }
    for (const auto field : {"recipes", "unresolved_recipes"})
    {
        // One entry per distinct recipe, at its first position, with the game
        // versions of every copy: a pipeline the base and an update share is
        // stored, and compiled, once.
        std::map<std::string, size_t> seen;
        std::vector<std::pair<std::string, std::vector<size_t>>> table; // body, contexts
        for (const auto & r : profile[field])
        {
            Writer recipe;
            Allowed(r, {"kind", "guest_words", "shader_hashes", "cbuf_sizes", "texture_pool", "environments", "shader_hash", "shared_memory_size", "workgroup_size", "texture_table_size", "reason", "source_context", "source_contexts"});
            Check(r["kind"].asString() == "compute" || r["kind"].asString() == "graphics", "Invalid recipe kind");
            const bool compute = r["kind"].asString() == "compute";
            const bool unresolved = std::string_view(field) == "unresolved_recipes";
            Check(!unresolved || (!compute && r["reason"].asString() == "no_serialized_environment_for_source_key"), "Invalid unresolved recipe");
            Check(unresolved || r["environments"].isArray(), "Invalid recipe environments");
            recipe.Var(compute ? 1 : 0);
            if (compute)
            {
                Check(r["workgroup_size"].isArray() && r["workgroup_size"].size() == 3 &&
                    r["cbuf_sizes"].isArray() && r["cbuf_sizes"].size() == 8, "Invalid compute recipe arrays");
                recipe.Var(Number(r["shared_memory_size"]));
                for (const auto & n : r["workgroup_size"])
                    recipe.Var(Number(n));
                for (const auto & n : r["cbuf_sizes"])
                    recipe.Var(Number(n));
                recipe.Var(Number(r["texture_table_size"]));
            }
            else
            {
                Check(r["shader_hashes"].isArray() && r["shader_hashes"].size() == 6 &&
                    r["cbuf_sizes"].isArray() && r["cbuf_sizes"].size() == 5 &&
                    r["texture_pool"].isArray() && r["texture_pool"].size() == 2, "Invalid graphics recipe arrays");
                recipe.Var(state_ids.at(Canonical(r["guest_words"])));
                // Resolved recipes derive their hashes from their programs.
                if (unresolved)
                    for (const auto & n : r["shader_hashes"])
                        recipe.Fixed(Number(n), 8);
                for (const auto & stage : r["cbuf_sizes"])
                {
                    Check(stage.isArray() && stage.size() == 18, "Invalid constant buffer array");
                    for (const auto & n : stage)
                        recipe.Var(Number(n));
                }
                for (const auto & n : r["texture_pool"])
                    recipe.Var(Number(n));
            }
            recipe.Var(r["environments"].size());
            for (const auto & id : r["environments"])
                recipe.Var(env_ids.at(id.asString()));
            const auto [found, inserted] = seen.emplace(recipe.bytes, table.size());
            if (inserted) table.emplace_back(std::move(recipe.bytes), std::vector<size_t>{});
            auto & indices = table[found->second].second;
            for (const auto & context : recipe_contexts(r))
                if (const auto index = context_index(context); std::find(indices.begin(), indices.end(), index) == indices.end())
                    indices.push_back(index);
        }
        out.Var(table.size());
        for (const auto & [body, indices] : table)
        {
            out.Var(indices.size());
            for (const auto index : indices)
                out.Var(index);
            out.bytes += body;
        }
    }
    Check(out.bytes.size() <= MaxProfilePayloadBytes, "Compact profile size limit");
    return Pack(out.bytes,compression_level);
}

namespace
{
// One NCA of the game versions the profile was exported from.
struct SourceFile
{
    std::string name;
    u64 size{};
    std::string header; // SHA-256 of its first 4 KiB.
};
// Finds the needed NCAs in any NSP or XCI directly inside root. Size and the hash
// of the first 4 KiB identify an NCA; every program read from it is still verified.
ContentMap LocateContents(const std::filesystem::path & root, u64 title_id, const std::vector<SourceFile> & sources,
                          const std::set<std::string> & needed, Timing::Metrics * timings, const ResourceProgress & progress)
{
    ContentMap found;
    std::vector<const SourceFile *> pending;
    for (const auto & source : sources)
        if (needed.contains(source.name)) pending.push_back(&source);
    if (pending.empty()) return found;
    std::vector<std::filesystem::path> containers;
    size_t entries{};
    std::error_code error;
    for (auto it = std::filesystem::directory_iterator(root, error); !error && it != std::filesystem::directory_iterator{}; it.increment(error))
    {
        Check(++entries <= 4096, "Local resource directory limit");
        std::error_code ignored;
        const auto name = it->path().filename().u8string();
        if (it->is_regular_file(ignored) && IsGameContainerName(std::string(name.begin(), name.end())))
            containers.push_back(it->path());
    }
    // A shared game folder holds many titles: dumps named after this one first.
    char title[17];
    std::snprintf(title, sizeof(title), "%016llx", static_cast<unsigned long long>(title_id));
    const auto named = [&](const std::filesystem::path & path) {
        const auto utf8 = path.filename().u8string();
        std::string name(utf8.begin(), utf8.end());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return c < 128 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c); });
        return name.find(std::string_view(title, 13)) != std::string::npos;
    };
    std::sort(containers.begin(), containers.end(), [&](const auto & a, const auto & b) {
        return std::make_pair(!named(a), a) < std::make_pair(!named(b), b);
    });
    for (const auto & path : containers)
    {
        if (pending.empty()) break;
        Check(!progress || progress(0, 1), "Shader profile operation cancelled");
        try
        {
            Timing::Timer timer(timings, Timing::Phase::ResourceIO);
            const auto length = std::filesystem::file_size(path);
            std::ifstream in(path, std::ios::binary);
            const auto read = [&](u64 off, u64 size) {
                Check(off <= length && size <= length - off && size <= 64 * 1024 * 1024, "Game container range");
                std::string bytes(static_cast<size_t>(size), '\0');
                in.clear();
                in.seekg(static_cast<std::streamoff>(off));
                in.read(bytes.data(), size);
                Check(bool(in), "Cannot read game container");
                return bytes;
            };
            for (const auto & entry : ContainerEntries(length, read))
            {
                std::string header;
                for (auto it = pending.begin(); it != pending.end();)
                {
                    if ((*it)->size != entry.size) { ++it; continue; }
                    if (header.empty()) header = Hash(read(entry.offset, std::min<u64>(entry.size, 4096)), timings);
                    if (header != (*it)->header) { ++it; continue; }
                    found[(*it)->name] = {path, entry.offset, entry.size};
                    it = pending.erase(it);
                }
            }
        }
        catch (const std::exception &) { /* Not a readable plaintext container. */ }
    }
    return found;
}
struct Pending
{
    Recipe recipe;
    std::vector<u32> ids;
    std::string content; // identity inputs after the context
};
// Same for a recipe whichever profile carries it, given the version it is used as.
std::string RecipeIdentity(const std::string & context, const std::string & content)
{
    Writer identity;
    identity.Text("nxshader-recipe-v1"); identity.Text(context);
    identity.bytes += content;
    return ResourceSha256(identity.bytes);
}
// Every table of a profile, checked and cross-referenced, before any local file is read.
struct Parsed
{
    std::string label, revision, note, source_context;
    bool full_game{};
    std::vector<GameVersion> versions; // One per context, in table order.
    std::vector<SourceFile> source_files;
    std::vector<Resource> resources;
    std::vector<Program> programs;
    std::vector<Env> envs;
    std::vector<Pending> pending;
    std::vector<Recipe> unresolved;
};
Parsed Parse(std::string_view payload, u64 title_id)
{
    Parsed parsed;
    Reader in{payload};
    const auto magic = in.Take(8);
    Check(magic == "NXSHBIN1" && in.Fixed(8) == title_id, "Compact title/version mismatch");
    parsed.label = in.Text();
    parsed.revision = in.Text(MaxRevision);
    parsed.note = in.Text(MaxNote);
    Check(ValidProfileText(parsed.label, 4096) && ValidProfileText(parsed.revision, MaxRevision) &&
          ValidProfileText(parsed.note, MaxNote), "Invalid profile description");
    const auto flags = in.Var();
    Check(flags <= 1, "Unknown profile flags");
    parsed.full_game = flags & 1;
    auto& source_files = parsed.source_files;
    std::set<std::string> source_names;
    auto& source_context = parsed.source_context;
    {
        const auto count = in.Count(1024);
        for (size_t i=0;i<count;++i) {
            SourceFile file{in.Text(), in.Var(), in.Hash()};
            Check(ValidResourceName(file.name) && source_names.insert(file.name).second, "Invalid source file name");
            source_files.push_back(std::move(file));
        }
    }
    std::vector<std::string> contexts;
    {
        const auto count = in.Count(65536);
        std::set<std::string> distinct;
        for (size_t i = 0; i < count; ++i) {
            contexts.push_back(in.Hash());
            Check(distinct.insert(contexts.back()).second, "Duplicate profile context");
        }
        for (const auto & context : contexts) {
            GameVersion version{context, in.Text(MaxVersionText), in.Text(MaxVersionText)};
            Check(ValidProfileText(version.base, MaxVersionText) && ValidProfileText(version.update, MaxVersionText), "Invalid profile version");
            parsed.versions.push_back(std::move(version));
        }
    }
    const auto read_context = [&] {
        const auto index = in.Var();
        Check(index <= contexts.size(), "Invalid profile context");
        return index ? contexts[index - 1] : std::string{};
    };
    source_context = read_context();
    const auto resource_count = in.Count(65536);
    auto& resources = parsed.resources;
    resources.reserve(resource_count);
    size_t total_spans{};
    for (size_t i = 0; i < resource_count; ++i)
    {
        Resource r;
        r.kind = in.Word();
        Check(r.kind < 5, "Compact resource kind");
        if (r.kind < 3) { r.file = in.Text(); Check(source_names.contains(r.file), "Compact resource outside the game contents"); }
        if (r.kind == 1 || r.kind == 2)
        {
            r.offset = in.Var();
            r.size = in.Var();
        }
        if (r.kind == 2)
        {
            r.decoded_size = in.Var();
            r.hash = in.Hash();
            r.segment_hash = in.Hash();
        }
        if (r.kind >= 3)
        {
            r.size = in.Var();
            r.hash = in.Hash();
            const auto count = in.Count(65536);
            total_spans += count;
            Check(total_spans <= 1000000, "Compact span budget");
            for (size_t n = 0; n < count; ++n)
                r.spans.push_back({in.Text(), in.Var(), in.Var()});
            u64 span_bytes{};
            for (const auto& span : r.spans) {
                Check(source_names.contains(span.file) && span.size <= r.size - span_bytes &&
                    span.offset <= UINT64_MAX - span.size, "Invalid compact span");
                span_bytes += span.size;
            }
            Check(span_bytes == r.size, "Incomplete compact spans");
        }
        Check(r.size <= MaxDecodedResource && r.decoded_size <= MaxDecodedResource, "Compact decoded limit");
        Check(r.offset <= UINT64_MAX - r.size, "Compact resource offset overflow");
        resources.push_back(std::move(r));
    }
    const auto program_count = in.Count(ProfileLimits{}.programs);
    auto& programs = parsed.programs;
    programs.reserve(program_count);
    std::set<u64> hashes;
    size_t code_budget{};
    for (size_t i = 0; i < program_count; ++i)
    {
        Program p;
        p.hash = in.Fixed(8);
        p.size = in.Var();
        Check(p.size && p.size % 8 == 0 && p.size <= 1024 * 1024 && p.hash && hashes.insert(p.hash).second, "Compact program bounds");
        const auto locator_count = in.Count(MaxProgramLocators);
        Check(locator_count != 0, "Compact program without locator");
        for (size_t n = 0; n < locator_count; ++n)
        {
            Locator l;
            l.resource = in.Word();
            l.offset = in.Var();
            Check(l.resource < resources.size(), "Compact program bounds");
            for (const auto& other : p.locators)
                Check(other.resource != l.resource || other.offset != l.offset, "Duplicate compact locator");
            const auto& resource = resources[l.resource];
            Check(l.offset <= UINT64_MAX - p.size, "Compact program offset overflow");
            if (resource.kind != 0) Check(l.offset <= MaxDecodedResource && p.size <= MaxDecodedResource - l.offset, "Compact program resource bounds");
            if (resource.kind == 2 || resource.kind == 3) {
                const auto limit = resource.kind == 2 ? resource.decoded_size : resource.size;
                Check(l.offset <= limit && p.size <= limit - l.offset, "Compact program resource range");
            }
            p.locators.push_back(l);
        }
        code_budget += p.size;
        Check(code_budget <= ProfileLimits{}.code_bytes, "Compact code budget");
        programs.push_back(std::move(p));
    }
    const auto env_count = in.Count(ProfileLimits{}.environments);
    auto& envs = parsed.envs;
    envs.reserve(env_count);
    size_t map_budget{};
    for (size_t i = 0; i < env_count; ++i)
    {
        Env env;
        env.program = in.Word();
        Check(env.program < programs.size(), "Compact environment program");
        for (size_t field = 0; field < env.fields.size(); ++field)
            if (field != ReadLowest && field != ReadHighest) env.fields[field] = in.Word();
        const auto start = u64{env.fields[StartAddress]}, size = programs[env.program].size;
        Check(size >= 8 && start + size - 8 <= UINT32_MAX, "Compact environment bounds");
        env.fields[ReadLowest] = env.fields[StartAddress];
        env.fields[ReadHighest] = static_cast<u32>(start + size - 8);
        Check(env.fields[6] <= 6, "Compact stage");
        Check(env.fields[2] == env.fields[3] && env.fields[4] >= env.fields[3] &&
            u64{env.fields[4]} - env.fields[3] + 8 == programs[env.program].size,
            "Compact environment bounds");
        Check(env.fields[6] == 5 || programs[env.program].size >= 80, "Compact graphics header size");
        for (size_t m = 0; m < 4; ++m)
        {
            const auto count = in.Count(65536);
            map_budget += count;
            Check(map_budget <= ProfileLimits{}.map_entries, "Compact map budget");
            u64 key{};
            for (size_t n = 0; n < count; ++n)
            {
                const auto gap = in.Var();
                Check(!n || (key < UINT64_MAX && gap <= UINT64_MAX - key - 1), "Compact environment map key");
                key = n ? key + 1 + gap : gap;
                Check(m >= 2 || key <= UINT32_MAX, "Compact environment map key");
                const auto value=in.Word();
                Check(ValidEnvironmentMapValue(m,value),"Invalid compact environment query value");
                env.maps[m].emplace_back(key, value);
            }
        }
        if (env.fields[6] == 5)
        {
            for (auto & n : env.workgroup)
                n = in.Word();
            env.shared = in.Word();
        }
        else
        {
            if (env.fields[6] == 3)
                for (auto & n : env.passthrough)
                    n = in.Word();
        }
        envs.push_back(std::move(env));
    }
    std::array<u32, 278> base;
    for (auto & n : base)
        n = in.Word();
    // Resolved and unresolved recipes may each bring their own guest state.
    const auto state_count = in.Count(ProfileLimits{}.recipes + ProfileLimits{}.unresolved);
    std::vector<std::vector<u32>> states;
    states.reserve(state_count);
    for (size_t i = 0; i < state_count; ++i)
    {
        std::vector<u32> state(base.begin(), base.end());
        const auto count = in.Count(278);
        size_t previous{};
        for (size_t n = 0; n < count; ++n)
        {
            const auto word = in.Word();
            Check(word < 278 && (n == 0 || word > previous), "Compact patch index");
            state[word] = in.Word();
            previous = word;
        }
        states.push_back(std::move(state));
    }
    auto& pending = parsed.pending;
    for (unsigned table = 0; table < 2; ++table)
    {
        const auto count = in.Count(table ? ProfileLimits{}.unresolved : ProfileLimits{}.recipes);
        for (size_t i = 0; i < count; ++i)
        {
            Pending p;
            auto & r = p.recipe;
            {
                const auto count = in.Count(contexts.size());
                for (size_t n = 0; n < count; ++n) {
                    const auto index = in.Var();
                    Check(index != 0 && index <= contexts.size(), "Invalid profile context");
                    Check(std::find(r.source_contexts.begin(), r.source_contexts.end(), contexts[index - 1]) == r.source_contexts.end(),
                          "Duplicate recipe context");
                    r.source_contexts.push_back(contexts[index - 1]);
                }
                // Until import chooses, the latest version recorded.
                if (!r.source_contexts.empty()) r.source_context = r.source_contexts.back();
            }
            const auto kind = in.Word();
            Check(kind <= 1 && (!table || kind == 0), "Compact recipe kind");
            r.compute = kind == 1;
            if (r.compute)
            {
                r.shared_memory_size = in.Word();
                for (auto & n : r.workgroup_size)
                    n = in.Word();
                for (auto & n : r.compute_cbuf_sizes)
                    n = in.Word();
                r.texture_table_size = in.Word();
            }
            else
            {
                const auto state = in.Word();
                Check(state < states.size(), "Compact state reference");
                r.guest_words = states[state];
                // Only unresolved recipes store hashes; the others derive them below.
                if (table)
                    for (auto & n : r.shader_hashes)
                        n = in.Fixed(8);
                for (auto & stage : r.cbuf_sizes)
                    for (auto & n : stage)
                        n = in.Word();
                for (auto & n : r.texture_pool)
                    n = in.Word();
            }
            const auto stages = in.Count(6);
            Check(table ? stages == 0 : (r.compute ? stages == 1 : stages >= 1), "Compact recipe stages");
            u64 previous_slot{};
            for (size_t j = 0; j < stages; ++j)
            {
                const auto id = in.Word();
                Check(id < envs.size() && r.compute == (envs[id].fields[6] == 5), "Compact environment reference");
                const auto slot = r.compute ? 0 : GraphicsSlot(envs[id].fields[6]);
                Check(slot < 6 && (j == 0 || slot > previous_slot), "Graphics recipe stage order mismatch");
                r.shader_hashes[slot] = programs[envs[id].program].hash;
                previous_slot = slot;
                p.ids.push_back(id);
            }
            if (!table) {
                if (r.compute) {
                    const auto& env = envs[p.ids.front()];
                    Check(env.workgroup == r.workgroup_size && env.shared == r.shared_memory_size,
                        "Compute recipe/environment dimensions mismatch");
                }
                size_t env_index{};
                for (size_t slot=0;slot<6;++slot) if (r.shader_hashes[slot]) {
                    const auto& env = envs[p.ids.at(env_index++)];
                    for (const auto& [key,value] : env.maps[2]) {
                        const auto bank = key >> 32;
                        if (static_cast<u32>(key) == UINT32_MAX && bank < (r.compute ? 8U : 18U)) {
                            const auto size = r.compute ? r.compute_cbuf_sizes[bank] : r.cbuf_sizes[slot <= 1 ? 0 : slot-1][bank];
                            Check(value == size, "Recipe constant buffer size mismatch");
                        }
                        if (r.compute && key == UINT64_MAX) Check(value == r.texture_table_size, "Recipe texture table size mismatch");
                        if (!r.compute && key == UINT64_MAX-1) Check(value == r.texture_pool[0], "Recipe texture pool mismatch");
                        if (!r.compute && key == UINT64_MAX-2) Check(value == r.texture_pool[1], "Recipe sampler pool mismatch");
                    }
                }
            }
            if (!table) pending.push_back(std::move(p));
            else parsed.unresolved.push_back(std::move(p.recipe));
        }
    }
    // Identity contains semantic compilation inputs only, never table indices,
    // physical locators, compression, labels or the community revision's digest.
    std::vector<std::string> environment_identities;
    environment_identities.reserve(envs.size());
    for (const auto& env : envs) {
        Writer identity;
        const auto words = [&](const auto& values) { for (auto value : values) identity.Fixed(value,4); };
        identity.Fixed(programs[env.program].hash,8); identity.Fixed(programs[env.program].size,8); words(env.fields);
        for (auto map : env.maps) {
            std::sort(map.begin(),map.end()); identity.Var(map.size());
            for (const auto& [key,value] : map) { identity.Fixed(key,8); identity.Fixed(value,4); }
        }
        words(env.workgroup); identity.Fixed(env.shared,4); words(env.passthrough);
        environment_identities.push_back(std::move(identity.bytes));
    }
    for (auto& p : pending) {
        Writer identity;
        const auto words = [&](const auto& values) { for (auto value : values) identity.Fixed(value,4); };
        identity.Var(p.recipe.compute); words(p.recipe.guest_words);
        for (auto hash : p.recipe.shader_hashes) identity.Fixed(hash,8);
        for (const auto& sizes : p.recipe.cbuf_sizes) words(sizes);
        words(p.recipe.texture_pool); identity.Fixed(p.recipe.shared_memory_size,4);
        words(p.recipe.workgroup_size); words(p.recipe.compute_cbuf_sizes); identity.Fixed(p.recipe.texture_table_size,4);
        for (auto id : p.ids) identity.bytes += environment_identities[id];
        p.content = std::move(identity.bytes);
        p.recipe.identity = RecipeIdentity(p.recipe.source_context, p.content);
    }
    Check(in.pos == payload.size(), "Trailing compact bytes");
    return parsed;
}
// Runs independent jobs on a few threads with at most Budget bytes in flight,
// unless one job alone needs more. Progress and cancellation stay on the
// calling thread; the first exception of a job is rethrown after all stop.
void RunBounded(size_t count, const std::vector<u64>& footprints, const ResourceProgress& progress,
                const std::function<void(size_t)>& job)
{
    constexpr u64 Budget = 768ULL << 20;
    const size_t threads = std::min<size_t>({count, std::max<size_t>(1, std::thread::hardware_concurrency()), 4});
    std::mutex mutex;
    std::condition_variable changed;
    size_t next{}, done{}, running{};
    u64 in_flight{};
    bool stop{}, cancelled{};
    std::exception_ptr failure;
    const auto worker = [&] {
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return stop || next == count || running == 0 || in_flight + footprints[next] <= Budget; });
            if (stop || next == count) return;
            const size_t index = next++;
            ++running;
            in_flight += footprints[index];
            lock.unlock();
            std::exception_ptr error;
            try { job(index); } catch (...) { error = std::current_exception(); }
            lock.lock();
            --running;
            in_flight -= footprints[index];
            ++done;
            if (error && !failure) { failure = error; stop = true; }
            changed.notify_all();
        }
    };
    std::vector<std::thread> pool;
    try {
        for (size_t i = 0; i < threads; ++i) pool.emplace_back(worker);
    } catch (const std::system_error&) { /* Fewer threads; at least the caller works. */ }
    if (pool.empty()) worker();
    {
        std::unique_lock lock(mutex);
        for (size_t reported = SIZE_MAX; ; ) {
            if (done != reported && !stop) {
                reported = done;
                lock.unlock();
                const bool keep = !progress || progress(reported, count);
                lock.lock();
                if (!keep) { stop = cancelled = true; changed.notify_all(); }
            }
            if (done == count || (stop && running == 0)) break;
            changed.wait(lock, [&] { return done != reported || done == count || (stop && running == 0); });
        }
    }
    for (auto& thread : pool) thread.join();
    if (failure) std::rethrow_exception(failure);
    Check(!cancelled, "Shader profile operation cancelled");
}
} // namespace
static std::vector<Recipe> ReadCompactImpl(std::string_view payload, const std::filesystem::path & root, u64 title_id, size_t * unresolved_count, Timing::Metrics * timings, const ResourceProgress & progress, bool metadata_only, ImportReport* report = nullptr, std::string_view target_context = {}, const std::set<std::string>* existing_recipes = nullptr, const ProgramMap* existing_programs = nullptr, bool allow_cross_version = false)
{
    Timing::Timer schema_timer(timings, Timing::Phase::SchemaAndLocators);
    auto parsed = Parse(payload, title_id);
    const auto& source_files = parsed.source_files;
    auto& resources = parsed.resources;
    const auto& programs = parsed.programs;
    const auto& envs = parsed.envs;
    auto& pending = parsed.pending;
    size_t total_environment_bytes{};
    if (unresolved_count) *unresolved_count = parsed.unresolved.size();
    // Parsing and all table references finish before touching local resources.
    schema_timer.Stop();
    if(metadata_only) {
        std::vector<Recipe> metadata;metadata.reserve(pending.size());
        for(auto& p:pending)metadata.push_back(std::move(p.recipe));
        return metadata;
    }
    ImportReport outcome;
    // Reject only incompatible recipe contexts. A merged community revision can
    // contain recipes from multiple updates without forcing irrelevant reads.
    std::set<size_t> needed_programs;
    // A recipe recorded with the running version is used as such; otherwise as
    // a candidate of the latest other version it was recorded with.
    std::vector<bool> compatible(pending.size());
    for (size_t i = 0; i < pending.size(); ++i) {
        auto& r = pending[i].recipe;
        const auto& contexts = r.source_contexts;
        if (target_context.empty()) compatible[i] = contexts.empty();
        else if (std::find(contexts.begin(), contexts.end(), target_context) != contexts.end()) {
            r.source_context = std::string(target_context);
            compatible[i] = true;
        } else if (allow_cross_version && ValidContext(target_context)) {
            const auto other = std::find_if(contexts.rbegin(), contexts.rend(), [](const auto& c) { return ValidContext(c); });
            if (other != contexts.rend()) { r.source_context = *other; compatible[i] = true; }
        }
        if (compatible[i]) r.identity = RecipeIdentity(r.source_context, pending[i].content);
    }
    for (size_t i = 0; i < pending.size(); ++i) {
        const auto& p = pending[i];
        if (!compatible[i]) { ++outcome.skipped; ++outcome.incompatible_contexts; continue; }
        if (existing_recipes && existing_recipes->contains(p.recipe.identity)) {
            ++outcome.reused; outcome.reused_ids.push_back(p.recipe.identity); continue;
        }
        for (auto id : p.ids) needed_programs.insert(envs[id].program);
    }
    std::vector<std::shared_ptr<const std::string>> codes(programs.size());
    std::vector<size_t> unresolved_programs;
    for (auto id : needed_programs) {
        if (existing_programs) {
            const auto found=existing_programs->find(programs[id].hash);
            if (found!=existing_programs->end() && found->second && found->second->size()==programs[id].size &&
                Verify(*found->second,programs[id].hash,timings)) codes[id]=found->second;
        }
        if (!codes[id]) unresolved_programs.push_back(id);
    }
    if (needed_programs.empty()) {
        if (report) *report = outcome;
        if (outcome.reused) return {};
        throw std::runtime_error("No recipes match the installed base/update identity; export a profile from an identified capture");
    }
    // Every resource a pass may read, for the alias search and the progress total.
    std::set<size_t> candidate_resources;
    for (auto id : unresolved_programs)
        for (const auto& locator : programs[id].locators) candidate_resources.insert(locator.resource);
    // Find the NCAs these resources name, in whichever containers hold them, then
    // still verify every requested program/resource. No guessed shader is accepted.
    ContentMap contents;
    {
        std::set<std::string> needed;
        for (const auto rid : candidate_resources) {
            const auto& resource=resources[rid];
            if (resource.kind<3) needed.insert(resource.file);
            for (const auto& span : resource.spans) needed.insert(span.file);
        }
        contents = LocateContents(root, title_id, source_files, needed, timings, progress ? ResourceProgress([&](size_t, size_t) {
            return progress(0, candidate_resources.size());
        }) : ResourceProgress{});
    }
    Timing::Timer resources_timer(timings, Timing::Phase::ResourceTotal);
    using Request = std::pair<size_t, u64>; // program, offset
    struct Job
    {
        size_t resource;
        std::vector<Request> requests;
        std::vector<size_t> missing, incompatible; // programs this job failed to load
    };
    std::vector<Job> finished;
    // Pass n tries the n-th locator of the programs still unresolved, so a program
    // shared by several game versions loads from whichever version is installed.
    for (size_t pass = 0; pass < MaxProgramLocators && !unresolved_programs.empty(); ++pass) {
        std::map<size_t, std::vector<Request>> groups;
        for (auto id : unresolved_programs)
            if (pass < programs[id].locators.size())
                groups[programs[id].locators[pass].resource].emplace_back(id, programs[id].locators[pass].offset);
        if (groups.empty()) break;
        // Groups are independent: each reads and decodes its own resource and fills
        // only its own programs. Largest first, so the longest decode starts first.
        std::vector<Job> jobs;
        std::vector<u64> footprints;
        for (auto& [rid, requests] : groups) {
            const auto& resource = resources[rid];
            // Raw bytes plus the decoded copy; Yaz0 decodes up to the last requested program.
            u64 prefix{};
            for (const auto& [id, offset] : requests) prefix = std::max(prefix, offset + programs[id].size);
            footprints.push_back(resource.kind == 0 ? 0 : resource.size + (resource.kind == 2 ? resource.decoded_size : resource.kind == 3 ? 0 : prefix));
            jobs.push_back({rid, std::move(requests), {}, {}});
        }
        {
            std::vector<size_t> order(jobs.size());
            std::iota(order.begin(), order.end(), size_t{});
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return footprints[a] > footprints[b]; });
            std::vector<Job> sorted_jobs;
            std::vector<u64> sorted_footprints;
            for (const auto i : order) { sorted_jobs.push_back(std::move(jobs[i])); sorted_footprints.push_back(footprints[i]); }
            jobs = std::move(sorted_jobs);
            footprints = std::move(sorted_footprints);
        }
        const size_t base = finished.size();
        const auto total = std::max(candidate_resources.size(), base + jobs.size());
        const ResourceProgress pass_progress = progress ? ResourceProgress([&](size_t done, size_t) {
            return progress(base + done, total);
        }) : ResourceProgress{};
        RunBounded(jobs.size(), footprints, pass_progress, [&](size_t index) {
            auto& job = jobs[index];
            const auto& resource = resources[job.resource];
            const auto unavailable = [&](bool missing, size_t id) {
                (missing ? job.missing : job.incompatible).push_back(id);
            };
            const auto all_unavailable = [&](bool missing) {
                for (const auto& [id, offset] : job.requests) if (!codes[id]) unavailable(missing, id);
            };
            if (resource.kind == 0) {
                try {
                    // Open the NCA's container once, then validate each requested
                    // range independently so a truncated tail cannot discard
                    // otherwise useful programs from the same content.
                    const auto& content=Locate(contents,resource.file);
                    const auto length=content.size;
                    std::ifstream input(content.path,std::ios::binary);
                    Check(bool(input),"Cannot open compact resource");
                    for (const auto& [id, offset] : job.requests) {
                        try {
                            const auto& p=programs[id];
                            Check(offset<=length && p.size<=length-offset,"Compact resource range");
                            std::string code(static_cast<size_t>(p.size),'\0');
                            {
                                Timing::Timer io(timings,Timing::Phase::ResourceIO);
                                input.clear(); input.seekg(static_cast<std::streamoff>(content.offset+offset));
                                input.read(code.data(),code.size());
                                Check(bool(input),"Cannot read compact resource");
                                if(timings)timings->Add(Timing::Counter::ResourceReadBytes,p.size);
                            }
                            Check(Verify(code,p.hash,timings),"Compact program fingerprint mismatch");
                            codes[id]=std::make_shared<const std::string>(std::move(code));
                        } catch (const std::filesystem::filesystem_error& error) {
                            unavailable(error.code()==std::errc::no_such_file_or_directory,id);
                        } catch (const std::runtime_error&) { unavailable(false, id); }
                    }
                } catch (const std::filesystem::filesystem_error& error) {
                    all_unavailable(error.code()==std::errc::no_such_file_or_directory);
                } catch (const std::runtime_error&) { all_unavailable(false); }
            } else {
                try {
                    size_t prefix{};
                    for (const auto& [id, offset] : job.requests) prefix = std::max(prefix, static_cast<size_t>(offset + programs[id].size));
                    std::string raw;
                    if (resource.kind >= 3) {
                        raw.reserve(resource.size);
                        for (const auto& span : resource.spans) raw += Bytes(contents, span.file, span.offset, span.size, timings);
                    } else raw = Bytes(contents, resource.file, resource.offset, resource.size, timings);
                    if (resource.kind >= 2) Check(Hash(raw, timings) == resource.hash, "Compact resource fingerprint mismatch");
                    auto decoded = Timing::Measure(timings, Timing::Phase::ResourceDecompression, [&] {
                        return resource.kind == 1 || resource.kind == 4 ? DecodeYaz0(raw, prefix) :
                            resource.kind == 2 ? DecodeLz4(raw, resource.decoded_size) : std::move(raw);
                    });
                    if (timings) timings->Add(Timing::Counter::ResourceDecodedBytes, decoded.size());
                    if (resource.kind == 2) Check(Hash(decoded, timings) == resource.segment_hash, "Compact NSO fingerprint mismatch");
                    for (const auto& [id, offset] : job.requests) {
                        const auto& p = programs[id];
                        if (offset > decoded.size() || p.size > decoded.size() - offset) { job.incompatible.push_back(id); continue; }
                        auto code = decoded.substr(offset, p.size);
                        if (!Verify(code, p.hash, timings)) { job.incompatible.push_back(id); continue; }
                        codes[id] = std::make_shared<const std::string>(std::move(code));
                    }
                } catch (const std::filesystem::filesystem_error& error) {
                    all_unavailable(error.code()==std::errc::no_such_file_or_directory);
                } catch (const std::runtime_error&) { all_unavailable(false); }
            }
        });
        std::erase_if(unresolved_programs, [&](size_t id) { return codes[id] != nullptr; });
        std::move(jobs.begin(), jobs.end(), std::back_inserter(finished));
    }
    if (progress) Check(progress(finished.size(), finished.size()), "Shader profile operation cancelled");
    // A resource counts as missing or incompatible only when a program it should
    // have provided stayed unresolved: another version's locator is not a failure.
    {
        const std::set<size_t> unresolved(unresolved_programs.begin(), unresolved_programs.end());
        const auto lost = [&](const std::vector<size_t>& ids) {
            return std::any_of(ids.begin(), ids.end(), [&](size_t id) { return unresolved.contains(id); });
        };
        for (const auto& job : finished) {
            outcome.missing_resources += lost(job.missing);
            outcome.incompatible_resources += lost(job.incompatible);
        }
    }
    resources_timer.Stop();
    Timing::Timer environment_timer(timings, Timing::Phase::EnvironmentAssembly);
    std::vector<SharedShaderBytes> assembled(envs.size());
    size_t env_budget{};
    for (size_t id=0;id<envs.size();++id)
    {
        const auto& env = envs[id];
        if (!codes[env.program]) continue;
        assembled[id] = Environment(env, codes[env.program], timings);
        env_budget += assembled[id].MetadataBytes();
        Check(env_budget <= 512 * 1024 * 1024, "Compact environment budget");
    }
    environment_timer.Stop();
    Timing::Timer recipe_timer(timings, Timing::Phase::RecipeAssembly);
    std::vector<Recipe> result;
    result.reserve(pending.size());
    for (size_t i = 0; i < pending.size(); ++i) {
        auto& p = pending[i];
        if (!compatible[i] || (existing_recipes && existing_recipes->contains(p.recipe.identity))) continue;
        bool available = true;
        for (auto id : p.ids) available = available && codes[envs[id].program];
        if (!available) { ++outcome.skipped; continue; }
        for (auto id : p.ids) {
            total_environment_bytes += sizeof(SharedShaderBytes);
            Check(total_environment_bytes <= 512 * 1024 * 1024, "Compact recipe memory budget");
            p.recipe.environments.push_back(assembled[id]);
        }
        p.recipe.requires_validation=p.recipe.source_context!=target_context;
        outcome.candidates+=p.recipe.requires_validation;
        result.push_back(std::move(p.recipe));
    }
    outcome.accepted = result.size();
    if (report) *report = outcome;
    Check(!result.empty() || outcome.reused, "No compatible shader recipes: local resources are missing or incompatible");
    if (timings)
    {
        timings->Add(Timing::Counter::Programs, programs.size());
        timings->Add(Timing::Counter::Environments, envs.size());
        timings->Add(Timing::Counter::Recipes, result.size());
        timings->Add(Timing::Counter::ResourceGroups, finished.size());
    }
    return result;
}
std::vector<Recipe> ReadCompact(std::string_view payload,const std::filesystem::path& root,u64 title_id,
    size_t* unresolved_count,Timing::Metrics* timings,const ResourceProgress& progress, ImportReport* report, std::string_view target_context, const std::set<std::string>* existing_recipes, const ProgramMap* existing_programs, bool allow_cross_version) {
    return ReadCompactImpl(payload,root,title_id,unresolved_count,timings,progress,false,report,target_context,existing_recipes,existing_programs,allow_cross_version);
}
ProfileInspection InspectCompact(std::string_view payload,u64 title_id) {
    const auto parsed=Parse(payload,title_id);
    ProfileInspection result;
    result.unresolved=parsed.unresolved.size();
    std::set<std::string> versions;
    for(const auto& p:parsed.pending) {
        if(p.recipe.compute)++result.compute;else ++result.graphics;
        versions.insert(p.recipe.source_contexts.begin(),p.recipe.source_contexts.end());
    }
    result.versions=versions.size();
    result.programs=parsed.programs.size();result.environments=parsed.envs.size();
    result.label=parsed.label;result.revision=parsed.revision;result.note=parsed.note;
    result.full_game=parsed.full_game;
    // The versions recipes still use, the exported one last; the others follow
    // the recipe order, which keeps the previous profile's versions first.
    for(const auto& version:parsed.versions)
        if(versions.contains(version.context)&&version.context!=parsed.source_context)result.targets.push_back(version);
    for(const auto& version:parsed.versions)
        if(version.context==parsed.source_context)result.targets.push_back(version);
    result.primary_context=parsed.source_context;
    if(!parsed.source_context.empty())versions.insert(parsed.source_context);
    result.contexts.assign(versions.begin(),versions.end());
    return result;
}
JsonValue DecodeCompact(std::string_view payload,u64 title_id) {
    const auto parsed=Parse(payload,title_id);
    const auto num=[](u64 n) { return n > UINT32_MAX ? JsonValue(std::to_string(n)) : JsonValue(static_cast<u32>(n)); };
    const auto array=[&](const auto& values) {
        JsonValue result(JsonValueType::Array);
        for(const auto n:values)result.Append(num(n));
        return result;
    };
    JsonValue profile(JsonValueType::Object);
    char title[17];
    std::snprintf(title,sizeof(title),"%016llx",static_cast<unsigned long long>(title_id));
    profile["format"]="nxshader";
    profile["version"]=2U;
    profile["guest_state_version"]=1U;
    profile["title_id"]=title;
    profile["content_label"]=parsed.label;
    profile["revision"]=parsed.revision;
    profile["note"]=parsed.note;
    if(parsed.full_game)profile["full_game"]=true;
    profile["versions"]=JsonValue(JsonValueType::Object);
    for(const auto& version:parsed.versions) {
        auto& entry=profile["versions"][version.context];
        entry["base"]=version.base;
        entry["update"]=version.update;
    }
    profile["source_context"]=parsed.source_context;
    profile["source_files"]=JsonValue(JsonValueType::Array);
    for(const auto& file:parsed.source_files) {
        JsonValue entry(JsonValueType::Object);
        entry["name"]=file.name;entry["size"]=num(file.size);entry["header"]=file.header;
        profile["source_files"].Append(std::move(entry));
    }
    // Programs have no local bytes here: a key derived from their hash and size
    // stands in for the source digest the exporter uses.
    std::vector<std::string> digests;
    profile["shaders"]=JsonValue(JsonValueType::Object);
    for(const auto& p:parsed.programs) {
        digests.push_back(ProgramKey(p.hash,p.size));
        auto& shader=profile["shaders"][digests.back()];
        shader["size"]=num(p.size);
        shader["references"]=JsonValue(JsonValueType::Array);
        for(const auto& l:p.locators) {
            const auto& resource=parsed.resources[l.resource];
            JsonValue locator(JsonValueType::Object);
            locator["encoding"]=Encodings[resource.kind];
            if(resource.kind<3)locator["file"]=resource.file;
            if(resource.kind==1 || resource.kind==2) {
                locator["container_offset"]=num(resource.offset);
                locator["container_size"]=num(resource.size);
            }
            if(resource.kind==2) {
                locator["decoded_size"]=num(resource.decoded_size);
                locator["container_sha256"]=resource.hash;
                locator["segment_sha256"]=resource.segment_hash;
            }
            if(resource.kind>=3) {
                locator["resource_size"]=num(resource.size);
                locator["resource_sha256"]=resource.hash;
                locator["physical_spans"]=JsonValue(JsonValueType::Array);
                for(const auto& span:resource.spans) {
                    JsonValue entry(JsonValueType::Object);
                    entry["file"]=span.file;entry["offset"]=num(span.offset);entry["size"]=num(span.size);
                    locator["physical_spans"].Append(std::move(entry));
                }
            }
            locator["offset"]=num(l.offset);
            locator["size"]=num(p.size);
            shader["references"].Append(std::move(locator));
        }
    }
    std::vector<std::string> env_ids;
    profile["environments"]=JsonValue(JsonValueType::Object);
    for(const auto& env:parsed.envs) {
        JsonValue e(JsonValueType::Object);
        e["shader"]=digests[env.program];
        for(size_t i=0;i<Fields.size();++i)e[Fields[i]]=num(env.fields[i]);
        for(size_t m=0;m<Maps.size();++m) {
            e[Maps[m]]=JsonValue(JsonValueType::Array);
            for(const auto& [key,value]:env.maps[m]) {
                JsonValue pair(JsonValueType::Array);
                pair.Append(num(key));pair.Append(num(value));
                e[Maps[m]].Append(std::move(pair));
            }
        }
        if(env.fields[6]==5) {
            e["workgroup_size"]=array(env.workgroup);
            e["shared_memory_size"]=num(env.shared);
        } else if(env.fields[6]==3) e["gp_passthrough_mask"]=array(env.passthrough);
        env_ids.push_back(ResourceSha256(Canonical(e)));
        profile["environments"][env_ids.back()]=std::move(e);
    }
    const auto recipe=[&](const Recipe& r,const std::vector<u32>* ids) {
        JsonValue result(JsonValueType::Object);
        result["kind"]=r.compute?"compute":"graphics";
        result["source_contexts"]=JsonValue(JsonValueType::Array);
        for(const auto& context:r.source_contexts)result["source_contexts"].Append(context);
        if(r.compute) {
            result["shader_hash"]=num(r.shader_hashes[0]);
            result["shared_memory_size"]=num(r.shared_memory_size);
            result["workgroup_size"]=array(r.workgroup_size);
            result["cbuf_sizes"]=array(r.compute_cbuf_sizes);
            result["texture_table_size"]=num(r.texture_table_size);
        } else {
            result["guest_words"]=array(r.guest_words);
            result["shader_hashes"]=array(r.shader_hashes);
            result["cbuf_sizes"]=JsonValue(JsonValueType::Array);
            for(const auto& sizes:r.cbuf_sizes)result["cbuf_sizes"].Append(array(sizes));
            result["texture_pool"]=array(r.texture_pool);
        }
        if(ids) {
            result["environments"]=JsonValue(JsonValueType::Array);
            for(const auto id:*ids)result["environments"].Append(env_ids[id]);
        } else result["reason"]="no_serialized_environment_for_source_key";
        return result;
    };
    profile["recipes"]=JsonValue(JsonValueType::Array);
    for(const auto& p:parsed.pending)profile["recipes"].Append(recipe(p.recipe,&p.ids));
    profile["unresolved_recipes"]=JsonValue(JsonValueType::Array);
    for(const auto& r:parsed.unresolved)profile["unresolved_recipes"].Append(recipe(r,nullptr));
    return profile;
}
ProfileInspection Inspect(const std::filesystem::path& path,u64 title_id) {
    const auto size=std::filesystem::file_size(path);
    Check(size<=MaxProfileFileBytes,"Profile size limit");
    std::ifstream file(path,std::ios::binary);std::string packed(static_cast<size_t>(size),'\0');
    Check(bool(file.read(packed.data(),static_cast<std::streamsize>(size))) && file.peek()==std::char_traits<char>::eof(),"Cannot read profile metadata");
    Check(packed.starts_with("NXSHADR2"),"Expected a compressed NXShader profile");
    auto result=InspectCompact(Unpack(packed),title_id);
    result.bytes=size;
    return result;
}
} // namespace VideoCommon::ShaderProfile
