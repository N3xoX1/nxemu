// SPDX-License-Identifier: GPL-2.0-or-later
#include "container_fixture.h"
#include "yuzu_video_core/shader_profile.h"
#include "yuzu_video_core/shader_profile_codec.h"
#include "yuzu_video_core/shader_profile_compact.h"
#include "yuzu_video_core/shader_profile_export.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_validation.h"
#include "video_settings.h"
#include "yuzu_common/logging/log.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <source_location>

VideoSettings videoSettings{};
namespace Common::Log {
LogLevel g_classLevel[static_cast<uint8_t>(LogClass::Count)]{};
void FmtLogMessageImpl(LogClass, LogLevel, const char*, unsigned int, const char*, const char*, const fmt::format_args&) {}
}
void assert_fail_impl() {throw std::runtime_error("renderer assertion");}
[[noreturn]] void unreachable_impl() {throw std::runtime_error("renderer unreachable");}
using namespace VideoCommon;
namespace Profile = ShaderProfile;
void Require(bool ok, const char* message, std::source_location where = std::source_location::current()) {
    if (!ok) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}
void Write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(bytes.data(),bytes.size());Require(bool(out),"Synthetic output failed");
}
struct Scratch {
    std::filesystem::path root=std::filesystem::temp_directory_path()/
        ("nxshader-merge-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch(){Require(std::filesystem::create_directory(root),"Fresh scratch required");}
    ~Scratch(){std::error_code error;std::filesystem::remove_all(root,error);}
};
constexpr u64 Title=0x0100152000022000;
// The exporter's representation: large values are strings.
u64 Number(const JsonValue& n){return n.isString() ? std::stoull(n.asString()) : n.asUInt64();}
JsonValue Num(u64 n){return n > UINT32_MAX ? JsonValue(std::to_string(n)) : JsonValue(static_cast<u32>(n));}
// One game version: an NCA named "main" holding its programs at their offsets,
// and one compute recipe per program, distinguished by its workgroup size.
struct Version {
    std::string context, file;
    std::vector<std::pair<std::string,u64>> programs; // code, offset
    JsonValue profile;
    Version(std::string context_, std::vector<std::pair<std::string,u64>> programs_, size_t file_size)
        : context(std::move(context_)), file(file_size, '.'), programs(std::move(programs_)) {
        for (const auto& [code, offset] : programs) file.replace(offset, code.size(), code);
        profile=Profile::ParseMetadata(R"({"format":"nxshader","version":2,"title_id":"0100152000022000","guest_state_version":1,"content_label":"synthetic","shaders":{},"environments":{},"recipes":[],"unresolved_recipes":[],"source_files":[]})");
        profile["source_context"]=context;
        JsonValue source(JsonValueType::Object);
        source["name"]="main";source["size"]=Num(file.size());
        source["header"]=Profile::ResourceSha256(std::string_view(file).substr(0,std::min<size_t>(file.size(),4096)));
        profile["source_files"].Append(source);
        for (const auto& [code, offset] : programs) Add(code, offset, Workgroup(code));
    }
    static u64 Workgroup(const std::string& code) { return 1 + static_cast<unsigned char>(code[0]) % 32; }
    void Add(const std::string& code, u64 offset, u64 workgroup) {
        const auto digest=Profile::ResourceSha256(code);
        JsonValue locator(JsonValueType::Object);
        locator["encoding"]="raw";locator["file"]="main";locator["offset"]=Num(offset);locator["size"]=Num(code.size());
        auto& shader=profile["shaders"][digest];
        shader["size"]=Num(code.size());
        shader["references"]=JsonValue(JsonValueType::Array);
        shader["references"].Append(locator);
        auto env=Profile::ParseMetadata(R"({"local_memory_size":0,"texture_bound":0,"guest_start_address":0,"guest_read_lowest":0,"viewport_transform_state":1,"stage":5,"texture_types":[],"texture_pixel_formats":[],"cbuf_values":[],"cbuf_replacements":[],"workgroup_size":[1,1,1],"shared_memory_size":0})");
        env["guest_read_highest"]=Num(code.size()-8);env["shader"]=digest;env["workgroup_size"][0u]=Num(workgroup);
        // Environment ids are a source detail; the merge must not depend on them.
        env["program_header_sha256"]=Profile::ResourceSha256(context);
        const auto env_id=context+"-"+digest+"-"+std::to_string(workgroup);
        profile["environments"][env_id]=env;
        auto recipe=Profile::ParseMetadata(R"({"kind":"compute","shared_memory_size":0,"workgroup_size":[1,1,1],"cbuf_sizes":[0,0,0,0,0,0,0,0],"texture_table_size":0,"environments":[]})");
        recipe["workgroup_size"][0]=Num(workgroup);
        recipe["shader_hash"]=Num(Common::CityHash64(code.data(),code.size()));
        recipe["environments"].Append(env_id);
        profile["recipes"].Append(recipe);
    }
    // What an export of this capture assembles before the merge.
    JsonValue Current() const { auto copy=profile; Profile::CanonicalizeEnvironments(copy); return copy; }
    std::string Payload() const { return Profile::Unpack(Profile::PackCompact(Current())); }
};
std::string Code(char tag) { return std::string(15, tag) + '!'; }
std::vector<Profile::Recipe> Import(std::string_view payload, const std::filesystem::path& root,
                                    const std::string& context, Profile::ImportReport& report) {
    return Profile::ReadCompact(payload, root, Title, nullptr, nullptr, {}, &report, context, nullptr, nullptr, true);
}
int main() try {
    Scratch scratch;
    {
        // Text fields: printable UTF-8 within the limit only.
        Require(Profile::ValidProfileText("", 4) && Profile::ValidProfileText("V1.0 full game", 32) &&
                Profile::ValidProfileText("Niveau 1 \xc3\xa9t\xc3\xa9 \xe2\x9c\x93", 64), "Valid text rejected");
        Require(!Profile::ValidProfileText("12345", 4), "Long text accepted");
        for (const std::string bad : {std::string("a\nb"), std::string("a\x7f"), std::string("\xc2\x85"),
                                      std::string("\xc0\xaf"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
                                      std::string("\xe2\x9c"), std::string(1, '\0')})
            Require(!Profile::ValidProfileText(bad, 64), "Invalid text accepted");
    }
    const auto x=Code('x'), y=Code('y'), z=Code('z');
    // The base holds X and Y, the update moved X and replaced Y with Z. Both
    // files are named "main" and have the same size, as on a real install.
    Version base(std::string(64,'a'), {{x,0},{y,32}}, 64);
    Version update(std::string(64,'b'), {{z,0},{x,48}}, 64);
    // Each export names the game version it was recorded with; a merge keeps both.
    base.profile["versions"][base.context]["base"]="1.0.0";
    update.profile["versions"][update.context]["base"]="1.0.0";
    update.profile["versions"][update.context]["update"]="4.0.1";
    update.profile["full_game"]=true;
    {
        // Canonical writer: map order, environment ids and duplicates do not change bytes.
        auto shuffled=base.profile;
        auto env=shuffled["environments"][base.context+"-"+Profile::ResourceSha256(x)+"-"+std::to_string(Version::Workgroup(x))];
        env["cbuf_values"]=Profile::ParseMetadata("[[8589934656,1],[8589934624,2]]");
        auto sorted=env;sorted["cbuf_values"]=Profile::ParseMetadata("[[8589934624,2],[8589934656,1]]");
        auto a=shuffled, b=shuffled;
        a["environments"]["custom"]=env; b["environments"]["other-id"]=sorted;
        auto recipe=a["recipes"][0u];recipe["environments"]=JsonValue(JsonValueType::Array);
        recipe["environments"].Append("custom");a["recipes"].Append(recipe);
        recipe["environments"]=JsonValue(JsonValueType::Array);recipe["environments"].Append("other-id");
        b["recipes"].Append(recipe);b["recipes"].Append(recipe);
        Require(Profile::PackCompact(a)==Profile::PackCompact(b), "Writer is not canonical");
        auto duplicated=Profile::ParseMetadata(R"({"cbuf_values":[[1,1],[1,2]]})");
        auto bad=a;bad["environments"]["custom"]["cbuf_values"]=duplicated["cbuf_values"];
        bool rejected{};try{Profile::PackCompact(bad);}catch(const std::exception&){rejected=true;}
        Require(rejected, "Duplicate map key accepted");
    }
    {
        // Decoding is the inverse of packing.
        const auto payload=base.Payload();
        Require(Profile::Unpack(Profile::PackCompact(Profile::DecodeCompact(payload,Title)))==payload, "Decode round trip changed bytes");
    }
    {
        // Re-exporting the installed profile with nothing new keeps it as is.
        auto current=base.Current();
        Require(Profile::MergeProfiles(Profile::DecodeCompact(base.Payload(),Title), current)==0, "Self merge inherited recipes");
        Require(Profile::Unpack(Profile::PackCompact(current))==base.Payload(), "Self merge changed the profile");
    }
    // The update also uses X with another pipeline the base never had.
    constexpr u64 UpdateOnly=30;
    update.Add(x,48,UpdateOnly);
    // An update player exports after importing the base player's profile.
    auto merged=update.Current();
    Require(Profile::MergeProfiles(Profile::DecodeCompact(base.Payload(),Title), merged)==1, "Base recipes not inherited");
    const auto payload=Profile::Unpack(Profile::PackCompact(merged));
    using Versions=std::vector<std::string>;
    {
        const auto info=Profile::InspectCompact(payload,Title);
        Require(info.compute==4 && info.programs==3 && info.versions==2, "Merged counts");
        Require(info.primary_context==update.context && info.contexts==Versions{base.context,update.context}, "Version identity inspection");
        Require(info.full_game && info.targets.size()==2 && Profile::GameVersionText(info.targets[0])=="1.0.0" &&
                Profile::GameVersionText(info.targets[1])=="1.0.0 + update 4.0.1", "Target versions");
        Require(!Profile::InspectCompact(base.Payload(),Title).full_game, "Whole game flag without the author");
        const auto profiles=scratch.root/"profiles";
        std::filesystem::create_directory(profiles);
        const auto base_path=profiles/"Game - Base [0100152000022000].nxshader";
        const auto combined_path=profiles/"Game - Combined [0100152000022000].nxshader";
        Write(base_path,Profile::PackCompact(base.Current()));
        Write(combined_path,Profile::PackCompact(merged));
        Require(Profile::FindProfileForContext({profiles},Title,base.context)==base_path,"Exact version discovery");
        Require(Profile::FindProfileForContext({profiles},Title,update.context)==combined_path,"Combined version discovery");
        Write(profiles/"0000000000002000.nxshader",Profile::PackCompact(merged));
        Require(Profile::FindProfileForContext({profiles},0x2000,update.context).empty(),"Foreign title discovery");
        // The base's recipes come first, in their order, then the update's. The
        // pipeline both versions use is one recipe listing both.
        const auto decoded=Profile::DecodeCompact(payload,Title);
        std::vector<std::pair<Versions,u64>> order;
        for (const auto& r : decoded["recipes"]) {
            Versions versions;
            for (const auto& v : r["source_contexts"]) versions.push_back(v.asString());
            order.emplace_back(versions, Number(r["workgroup_size"][0u]));
        }
        const std::vector<std::pair<Versions,u64>> expected{
            {{base.context,update.context},Version::Workgroup(x)},{{base.context},Version::Workgroup(y)},
            {{update.context},Version::Workgroup(z)},{{update.context},UpdateOnly}};
        Require(order==expected, "Merged recipe order");
        // X, in both versions, keeps both locators: the update's, then the base's renamed file.
        const auto& shader=decoded["shaders"][Profile::ProgramKey(Common::CityHash64(x.data(),x.size()),x.size())];
        Require(shader["references"].size()==2 && shader["references"][0u]["file"].asString()=="main" &&
                Number(shader["references"][0u]["offset"])==48 &&
                shader["references"][1u]["file"].asString()=="main~1" &&
                Number(shader["references"][1u]["offset"])==0, "Shared program locators");
        Require(decoded["source_files"].size()==2, "Source files of both versions");
        // Packing what was decoded changes nothing.
        Require(Profile::Unpack(Profile::PackCompact(decoded))==payload, "Multi-version round trip");
    }
    for (const auto* player : {&base, &update}) {
        // Each player finds every program of their own version. The shared
        // pipeline is theirs, not a candidate; only the other version's own
        // pipelines on shared programs are candidates to validate.
        const bool on_base=player==&base;
        const auto root=scratch.root/player->context.substr(0,1);
        std::filesystem::create_directory(root);
        Write(root/"game.dnsp", ContainerFixture::Nsp({{"cnmt.nca", "metadata"}, {"main", player->file}}));
        Profile::ImportReport report;
        const auto recipes=Import(payload, root, player->context, report);
        {
            // The same content dumped as an XCI loads the same programs.
            const auto card=scratch.root/(player->context.substr(0,1)+"-xci");
            std::filesystem::create_directory(card);
            Write(card/"Game [cartridge].dxci", ContainerFixture::Xci({{"main", player->file}, {"cnmt.nca", "metadata"}}));
            Profile::ImportReport from_card;
            const auto card_recipes=Import(payload, card, player->context, from_card);
            Require(card_recipes.size()==recipes.size() && from_card.missing_resources==report.missing_resources &&
                    from_card.incompatible_resources==report.incompatible_resources, "XCI dump of the same version");
            for (size_t i=0;i<recipes.size();++i)
                Require(card_recipes[i].identity==recipes[i].identity &&
                        card_recipes[i].environments[0].ProgramBytes()==recipes[i].environments[0].ProgramBytes(), "XCI program bytes");
        }
        const auto other=on_base ? z : y;
        Require(recipes.size()==3 && report.candidates==(on_base ? 1U : 0U) &&
                report.missing_resources+report.incompatible_resources==1, "Player import of a merged profile");
        size_t shared{};
        for (const auto& r : recipes) {
            Require(r.environments[0].ProgramBytes()!=other, "Program of the other version loaded");
            Require(r.requires_validation==(r.source_context!=player->context), "Foreign recipe skipped validation");
            if (r.workgroup_size[0]==Version::Workgroup(x)) {
                ++shared;
                Require(!r.requires_validation && r.source_context==player->context, "Shared recipe not used as own");
            }
        }
        Require(shared==1, "Shared pipeline imported twice");
        // Their own programs alone: nothing of theirs is reported missing.
        Profile::ImportReport own;
        auto mine=Profile::Unpack(Profile::PackCompact(player->Current()));
        Require(Import(mine, root, player->context, own).size()==player->profile["recipes"].size() &&
                !own.missing_resources && !own.incompatible_resources, "Own profile import");
    }
    {
        // Over the limits, the oldest version goes first, never the one played:
        // the base loses Y, and the shared pipeline stays for the update only.
        auto trimmed=merged;
        Profile::ProfileLimits limits;
        limits.recipes=3;
        const auto report=Profile::TrimToLimits(trimmed, update.context, limits);
        Require(report.versions==1 && report.recipes==1 && trimmed["recipes"].size()==3, "Oldest version trimmed");
        const auto info=Profile::InspectCompact(Profile::Unpack(Profile::PackCompact(trimmed)),Title);
        Require(info.versions==1 && info.programs==2, "Trimmed profile tables");
        // The version played is kept whole even when it alone exceeds them.
        limits.recipes=1;
        trimmed=merged;
        Require(Profile::TrimToLimits(trimmed, update.context, limits).versions==1 && trimmed["recipes"].size()==3,
                "Played version trimmed");
        // Within the limits nothing changes.
        trimmed=merged;
        Require(Profile::TrimToLimits(trimmed, update.context, {}).versions==0 &&
                Profile::PackCompact(trimmed)==Profile::PackCompact(merged), "Profile within limits trimmed");
    }
    {
        // A previous program with the same hash but another size is a different
        // program: it is dropped with its recipes, never mixed with ours.
        auto previous=Profile::DecodeCompact(base.Payload(),Title);
        const auto key=Profile::ProgramKey(Common::CityHash64(x.data(),x.size()),x.size());
        auto shader=previous["shaders"][key];
        previous["shaders"].removeMember(key.c_str());
        shader["size"]=Num(24);shader["references"][0u]["size"]=Num(24);
        previous["shaders"]["conflict"]=shader;
        for (const auto& id : previous["environments"].GetMemberNames())
            if (previous["environments"][id.c_str()]["shader"].asString()==key) {
                previous["environments"][id.c_str()]["shader"]="conflict";
                previous["environments"][id.c_str()]["guest_read_highest"]=Num(16);
            }
        auto current=update.Current();
        Require(Profile::MergeProfiles(previous, current)==1, "Conflicting program kept");
        Require(Profile::InspectCompact(Profile::Unpack(Profile::PackCompact(current)),Title).programs==3, "Conflict programs");
    }
    {
        // An observation stays unresolved once, and not at all once resolved.
        auto unresolved=Profile::ParseMetadata(R"({"kind":"graphics","reason":"no_serialized_environment_for_source_key","cbuf_sizes":[],"texture_pool":[0,0],"shader_hashes":[0,0,0,0,0,7],"environments":[],"guest_words":[]})");
        for (unsigned i=0;i<5;++i) unresolved["cbuf_sizes"].Append(JsonValue(JsonValueType::Array));
        for (unsigned i=0;i<5;++i) for (unsigned n=0;n<18;++n) unresolved["cbuf_sizes"][i].Append(Num(0));
        for (unsigned i=0;i<278;++i) unresolved["guest_words"].Append(Num(i==0 ? 63 : i==1 ? 1 : 0));
        unresolved["source_context"]=update.context;
        auto previous_profile=base.Current();
        previous_profile["unresolved_recipes"].Append(unresolved);
        auto current=update.Current();
        current["unresolved_recipes"].Append(unresolved);
        Profile::MergeProfiles(Profile::DecodeCompact(Profile::Unpack(Profile::PackCompact(previous_profile)),Title), current);
        Require(current["unresolved_recipes"].size()==1, "Unresolved observation duplicated");
    }
    std::cout << "PASS: profile text, canonical writer, decode round trip, self merge, "
                 "base/update merge with shared locators and multi-version recipes, trimming, conflicts, unresolved dedup\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
