// SPDX-License-Identifier: GPL-2.0-or-later
#include "container_fixture.h"
#include "yuzu_video_core/pipeline_cache_store.h"
#include "yuzu_video_core/shader_profile.h"
#include "yuzu_video_core/shader_profile_codec.h"
#include "yuzu_video_core/shader_profile_compact.h"
#include "yuzu_video_core/shader_profile_export.h"
#include "yuzu_video_core/shader_profile_hash.h"
#include "yuzu_video_core/shader_profile_validation.h"
#include "video_settings.h"
#include "yuzu_common/logging/log.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <source_location>
#include <thread>

VideoSettings videoSettings{};
namespace Common::Log {
LogLevel g_classLevel[static_cast<uint8_t>(LogClass::Count)]{};
void FmtLogMessageImpl(LogClass, LogLevel, const char*, unsigned int, const char*, const char*, const fmt::format_args&) {}
}
void assert_fail_impl() {throw std::runtime_error("renderer assertion");}
[[noreturn]] void unreachable_impl() {throw std::runtime_error("renderer unreachable");}
using namespace VideoCommon;
namespace Profile = ShaderProfile;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void Reject(F fn, std::source_location where = std::source_location::current()) {
    bool rejected{};try { fn(); } catch (const std::exception&) { rejected=true; }
    if (!rejected) throw std::runtime_error("Malformed synthetic input accepted at line "+std::to_string(where.line()));
}
void Put(std::string& bytes, size_t offset, u64 value, size_t length) {
    for (size_t i=0;i<length;++i) bytes.at(offset+i)=static_cast<char>(value>>(8*i));
}
void Write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(bytes.data(),bytes.size());Require(bool(out),"Synthetic output failed");
}
// A dump holding one NCA with these bytes.
std::string Game(const std::string& nca) { return ContainerFixture::Nsp({{"game.nca", nca}}); }
std::string Read(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);return {(std::istreambuf_iterator<char>(in)),{}};
}
struct Scratch {
    std::filesystem::path root=std::filesystem::temp_directory_path()/
        ("nxshader-robust-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch(){Require(std::filesystem::create_directory(root),"Fresh scratch required");}
    ~Scratch(){std::error_code error;std::filesystem::remove_all(root,error);}
};
int main() try {
    Scratch scratch;
    std::mt19937 random(918);
    const std::string code="synthetic-bytes!";
    const auto digest=Profile::ResourceSha256(code);
    const auto hash=Common::CityHash64(code.data(),code.size());
    auto profile=Profile::ParseMetadata(R"({"format":"nxshader","version":2,"title_id":"0100152000022000","guest_state_version":1,"content_label":"synthetic","shaders":{},"environments":{},"recipes":[],"unresolved_recipes":[]})");
    auto shader=Profile::ParseMetadata(R"({"size":16,"references":[{"encoding":"raw","file":"game.nca","offset":0,"size":16}]})");
    profile["source_files"]=Profile::ParseMetadata(R"([{"name":"game.nca","size":16}])");
    profile["source_files"][0u]["header"]=Profile::ResourceSha256(code);
    profile["shaders"][digest]=shader;
    auto env=Profile::ParseMetadata(R"({"local_memory_size":0,"texture_bound":0,"guest_start_address":0,"guest_read_lowest":0,"guest_read_highest":8,"viewport_transform_state":1,"stage":5,"texture_types":[],"texture_pixel_formats":[],"cbuf_values":[],"cbuf_replacements":[],"workgroup_size":[1,1,1],"shared_memory_size":0})");
    env["shader"]=digest;profile["environments"]["env"]=env;
    auto recipe=Profile::ParseMetadata(R"({"kind":"compute","shared_memory_size":0,"workgroup_size":[1,1,1],"cbuf_sizes":[0,0,0,0,0,0,0,0],"texture_table_size":0,"environments":["env"]})");
    recipe["shader_hash"]=std::to_string(hash);profile["recipes"].Append(recipe);
    const auto packed=Profile::PackCompact(profile),payload=Profile::Unpack(packed);
    constexpr u64 title=0x0100152000022000;
    Write(scratch.root/"game.dnsp",Game(code));
    auto recipes=Profile::ReadCompact(payload,scratch.root,title,nullptr,nullptr);
    Require(recipes.size()==1 && recipes[0].environments[0].ProgramBytes()==code,"Exact program coverage lost");
    Require(recipes[0].shader_hashes[0]==hash,"Recipe hash not derived from its program");
    {
        // A game the user keeps elsewhere and links into the directory is read.
        const auto library=scratch.root/"library",linked=scratch.root/"linked";
        std::filesystem::create_directories(library);std::filesystem::create_directories(linked);
        Write(library/"game.dnsp",Game(code));
        // Windows needs a privilege or Developer Mode for symlinks: skip, do not fail.
        std::error_code linking;
        std::filesystem::create_symlink(library/"game.dnsp",linked/"game.dnsp",linking);
        if(linking) std::cerr<<"Symlink check skipped: "<<linking.message()<<'\n';
        else {
            Require(Profile::ReadCompact(payload,linked,title,nullptr,nullptr).size()==1,"Linked game file was rejected");
            std::filesystem::remove(linked/"game.dnsp");
            std::filesystem::create_directory_symlink(library,linked/"game.dnsp");
            Reject([&]{Profile::ReadCompact(payload,linked,title,nullptr,nullptr);});
        }
    }
    {
        // Programs take their hash from the recipes using them: a contradicting
        // hash is refused and environments no recipe uses are not stored.
        auto conflict=profile;auto other=recipe;other["shader_hash"]=std::to_string(hash+1);
        other["workgroup_size"][0]=2;conflict["recipes"].Append(other);
        Reject([&]{Profile::PackCompact(conflict);});
        auto unused=profile;auto spare=env;spare["local_memory_size"]=16;unused["environments"]["unused"]=spare;
        Require(Profile::PackCompact(unused)==packed,"Unused environment was stored");
    }
    // Exercise actual profile import in both directions, base -> update and
    // update -> base. Matching bytes remain candidates until runtime dependency
    // validation; a context match alone never authorizes changed shader bytes.
    const std::array<std::string, 2> contexts{std::string(64, 'a'), std::string(64, 'b')};
    for (size_t source = 0; source < contexts.size(); ++source) {
        auto versioned = profile;
        versioned["source_context"] = contexts[source];
        const auto version_payload = Profile::Unpack(Profile::PackCompact(versioned));
        for (size_t target = 0; target < contexts.size(); ++target) {
            Profile::ImportReport report;
            auto imported = Profile::ReadCompact(version_payload, scratch.root, title,
                nullptr, nullptr, {}, &report, contexts[target], nullptr, nullptr, true);
            Require(imported.size() == 1, "Bidirectional update import lost identical shader");
            Require(imported[0].requires_validation == (source != target),
                    "Foreign shader bypassed dependency validation");
            Require(report.candidates == (source != target), "Incorrect update candidate count");
            Write(scratch.root / "game.dnsp", Game(std::string(code.size(), 'X')));
            Reject([&] { Profile::ReadCompact(version_payload, scratch.root, title,
                nullptr, nullptr, {}, nullptr, contexts[target], nullptr, nullptr, true); });
            std::filesystem::remove(scratch.root / "game.dnsp");
            Reject([&] { Profile::ReadCompact(version_payload, scratch.root, title,
                nullptr, nullptr, {}, nullptr, contexts[target], nullptr, nullptr, true); });
            Write(scratch.root / "game.dnsp", Game(code));
        }
        Reject([&] { Profile::ReadCompact(version_payload, scratch.root, title,
            nullptr, nullptr, {}, nullptr, {}, nullptr, nullptr, true); });
        Reject([&] { Profile::ReadCompact(version_payload, scratch.root, title,
            nullptr, nullptr, {}, nullptr, contexts[1 - source]); });
    }
    for(size_t cut=0;cut<payload.size();++cut)
        Reject([&]{Profile::InspectCompact(std::string_view(payload).substr(0,cut),title);});
    // Envelope mutations must be rejected even when an altered magic no longer
    // enters the decompressor: the strict payload parser still rejects it. Zstd
    // ignores a few encoding bits; such a flip may only yield the same payload.
    for(size_t i=0;i<packed.size();++i) {
        auto bad=packed;bad[i]^=static_cast<char>(1U<<(random()%8));
        std::string accepted;
        try { accepted=Profile::Unpack(bad); Profile::InspectCompact(accepted,title); } catch (const std::exception&) { continue; }
        Require(accepted==payload,"Envelope mutation changed an accepted payload");
    }
    size_t rejected{};
    for(unsigned iteration=0;iteration<3000;++iteration) {
        auto bad=payload;
        for(unsigned n=0;n<1+iteration%3;++n)bad[random()%bad.size()]^=static_cast<char>(1U<<(random()%8));
        try { Profile::InspectCompact(bad,title); }catch(const std::exception&){++rejected;}
    }
    Require(rejected>0,"Mutation corpus did not exercise structural rejection");
    // Many valid compute variants retain their distinct compiler inputs while
    // sharing one program, including the writer's immutable-program memo path.
    auto variants=profile;
    variants["recipes"]=JsonValue(JsonValueType::Array);
    variants["environments"]=JsonValue(JsonValueType::Object);
    for(unsigned i=0;i<256;++i) {
        const auto name="env-"+std::to_string(i);
        auto environment=env;environment["workgroup_size"][0]=i+1;
        variants["environments"][name]=environment;
        auto r=recipe;r["workgroup_size"][0]=i+1;r["environments"][0]=name;
        variants["recipes"].Append(r);
    }
    const auto variant_recipes=Profile::ReadCompact(Profile::Unpack(Profile::PackCompact(variants)),scratch.root,title,nullptr,nullptr);
    Require(variant_recipes.size()==256,"Compute variants lost during import");
    const auto variants_cache=scratch.root/"variants.nxc";
    for(const auto& r:variant_recipes) {
        Require(r.environments[0].ProgramIdentity()==variant_recipes[0].environments[0].ProgramIdentity(),"Variant program duplicated");
        CacheStore::Append(variants_cache,20,{r.environments,std::to_string(r.workgroup_size[0])});
    }
    Require(CacheStore::Read(variants_cache,20).size()==256,"Compute variants lost in local cache");
    // A falsely labelled cached program must never bypass verification. It may
    // fall back to local game bytes, but cannot work after those bytes disappear.
    Profile::ProgramMap available;
    available[hash]=std::make_shared<const std::string>(code.size(),'X');
    Require(Profile::ReadCompact(payload,scratch.root,title,nullptr,nullptr,{},nullptr,{},nullptr,&available).size()==1,"Bad cache prevented valid resource fallback");
    std::filesystem::remove(scratch.root/"game.dnsp");
    Reject([&]{Profile::ReadCompact(payload,scratch.root,title,nullptr,nullptr,{},nullptr,{},nullptr,&available);});
    available[hash]=std::make_shared<const std::string>(code);
    Require(Profile::ReadCompact(payload,scratch.root,title,nullptr,nullptr,{},nullptr,{},nullptr,&available).size()==1,"Verified local program was not reused");
    const std::set<std::string> existing{recipes[0].identity};Profile::ImportReport reused;
    Require(Profile::ReadCompact(payload,scratch.root/"absent",title,nullptr,nullptr,{},&reused,{},&existing).empty() && reused.reused==1,"Verified recipe reuse performed resource IO");
    // Hash conventions and every prefix length are explicit, without accepting
    // arbitrary aliases for a program with the same SHA-256.
    std::string terminal(8,'\0');Put(terminal,0,0xE2400FFFFF87000FULL,8);
    Require(Profile::MatchesProgramHash(code+terminal,hash),"Analyze terminal hash lost");
    Require(Profile::MatchesProgramHash(code+terminal,Common::CityHash64((code+terminal).data(),24)),"CFG hash lost");
    Require(!Profile::MatchesProgramHash(code,hash^1),"Arbitrary program alias accepted");
    // Random append sequence: all combinations survive, repeats add no bytes,
    // and all copies of each program/environment share their immutable storage.
    const auto cache=scratch.root/"random.nxc";
    std::map<std::string,CacheStore::Record> expected;
    std::vector<std::string> envs;
    for(unsigned i=0;i<24;++i) {
        std::string bytes(68+128+16,'\0');Put(bytes,0,128,8);Put(bytes,68,i%4,8);Put(bytes,196,i,8);
        envs.push_back(std::move(bytes));
    }
    for(unsigned i=0;i<1200;++i) {
        CacheStore::Record record;
        const auto key=random()%240;record.key=std::to_string(key);
        for(unsigned n=0;n<1+key%6;++n)record.environments.emplace_back(envs[(key+n)%24]);
        expected.emplace(record.key,record);CacheStore::Append(cache,20,record);
    }
    const auto bytes=Read(cache);
    for(const auto& [key,record]:expected)CacheStore::Append(cache,20,record);
    Require(Read(cache)==bytes,"Duplicate append grew the cache");
    auto stored=CacheStore::Read(cache,20);
    Require(stored.size()==expected.size(),"Pipeline combination lost");
    std::map<std::string,const void*> identities;
    for(const auto& record:stored) {
        Require(record.environments==expected.at(record.key).environments,"Roundtrip inputs changed");
        for(const auto& environment:record.environments) {
            auto [it,fresh]=identities.emplace(environment.String(),environment.Identity());
            Require(fresh || it->second==environment.Identity(),"Environment not shared");
        }
    }
    // Every cut in a small valid cache: only complete records are exposed;
    // append then repairs the torn tail and restores the exact same record.
    const auto small=scratch.root/"small.nxc",torn=scratch.root/"torn.nxc";
    CacheStore::Record one{{envs[0]},"one"};CacheStore::Append(small,20,one);
    const auto complete=Read(small);
    for(size_t cut=16;cut<complete.size();++cut) {
        Write(torn,std::string_view(complete).substr(0,cut));
        Require(CacheStore::Read(torn,20).empty(),"Incomplete pipeline was exposed");
        CacheStore::Append(torn,20,one);
        Require(Read(torn)==complete,"Torn-tail recovery changed records");
    }
    // Corrupt every data byte of a complete frame. A bad checksum is never
    // exposed and read never alters the source; the frame ends the valid prefix
    // and the next append drops it and rewrites the exact same record.
    size_t cursor=16,flips{};
    while(cursor<complete.size()) {
        size_t length{};for(size_t i=0;i<4;++i)length|=size_t(static_cast<unsigned char>(complete[cursor+1+i]))<<(8*i);
        for(size_t offset=cursor+37;offset<cursor+37+length;++offset) {
            auto bad=complete;bad[offset]^=1;Write(torn,bad);
            Require(CacheStore::Read(torn,20).empty(),"Corrupt pipeline was exposed");
            Require(Read(torn)==bad,"Read changed corrupt source");++flips;
            CacheStore::Append(torn,20,one);
            Require(Read(torn)==complete,"Corrupt-frame recovery changed records");
        }
        cursor+=37+length;
    }
    // A cache restored/replaced with an equally-sized file must invalidate the
    // writer's ID index, even though its byte count did not change.
    const auto replacement=scratch.root/"replacement.nxc";
    auto other=one;other.key="two";CacheStore::Append(replacement,20,other);
    CacheStore::Append(small,20,one); // Prime this path's in-process writer.
    const auto modified=std::filesystem::last_write_time(small);
    Write(small,Read(replacement));
    std::filesystem::last_write_time(small,modified+std::chrono::seconds(2));
    CacheStore::Append(small,20,one);
    Require(CacheStore::Read(small,20).size()==2,"Same-size cache replacement hid a pipeline");
    const std::string context(64,'a');
    CacheStore::Append(replacement,20,other,nullptr,context);
    CacheStore::Append(small,20,one,nullptr,context);
    const auto context_path=CacheStore::ContextIndexPath(small,context);
    const auto replacement_context=CacheStore::ContextIndexPath(replacement,context);
    const auto context_modified=std::filesystem::last_write_time(context_path);
    Write(context_path,Read(replacement_context));
    std::filesystem::last_write_time(context_path,context_modified+std::chrono::seconds(2));
    CacheStore::Append(small,20,one,nullptr,context);
    Require(CacheStore::ReadContext(small,context).size()==2,"Same-size context replacement hid a pipeline");
    // The same with an unchanged write time, which a rewrite within one clock tick keeps.
    const auto same=scratch.root/"same.nxc";
    CacheStore::Append(same,20,one,nullptr,context);
    const auto same_context=CacheStore::ContextIndexPath(same,context);
    for(const auto& [target,source]:{std::pair{same,replacement},std::pair{same_context,replacement_context}}) {
        const auto unchanged=std::filesystem::last_write_time(target);
        Write(target,Read(source));std::filesystem::last_write_time(target,unchanged);
    }
    CacheStore::Append(same,20,one,nullptr,context);
    Require(CacheStore::Read(same,20).size()==2,"Same-time cache replacement hid a pipeline");
    Require(CacheStore::ReadContext(same,context).size()==2,"Same-time context replacement hid a pipeline");
    // Weak program-ID memoization must not reuse a digest after its owner dies.
    const auto owners=scratch.root/"owners.nxc";
    for(unsigned i=0;i<256;++i) {
        auto program=std::make_shared<const std::string>(128,static_cast<char>(i));
        auto metadata=envs[0].substr(0,68)+envs[0].substr(196);
        auto environment=SharedShaderBytes::FromMetadata(std::move(metadata),program);
        CacheStore::Append(owners,20,{{std::move(environment)},std::to_string(i)});
    }
    const auto owned_records=CacheStore::Read(owners,20);
    Require(owned_records.size()==256,"Expired program owner lost a record");
    for(unsigned i=0;i<256;++i)
        Require(owned_records[i].environments[0].ProgramBytes()==std::string(128,static_cast<char>(i)),"Expired owner reused a wrong digest");
    // Multiple threads use the production writer and request overlapping keys.
    const auto concurrent=scratch.root/"threads.nxc";
    std::atomic<bool> failed{};std::vector<std::thread> threads;
    for(unsigned t=0;t<8;++t)threads.emplace_back([&,t]{try {
        for(unsigned i=0;i<100;++i)CacheStore::Append(concurrent,20,{{envs[(i+t)%24]},std::to_string(i)+":"+std::to_string((i+t)%24)});
    }catch(...){failed=true;}});
    for(auto& thread:threads)thread.join();
    Require(!failed && CacheStore::Read(concurrent,20).size()==800,"Concurrent writer lost entries");
    std::cout<<"{\"payload_truncations\":"<<payload.size()<<",\"envelope_mutations\":"<<packed.size()
        <<",\"payload_mutations\":3000,\"rejected_mutations\":"<<rejected
        <<",\"compute_variants\":256,\"random_appends\":1200,\"verified_combinations\":"<<stored.size()
        <<",\"cache_truncations\":"<<complete.size()-16<<",\"cache_data_corruptions\":"<<flips
        <<",\"concurrent_appends\":800}\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
