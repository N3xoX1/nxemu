// SPDX-License-Identifier: GPL-2.0-or-later
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <source_location>
#include <string>
#include "yuzu_video_core/pipeline_cache_store.h"
#include "yuzu_video_core/shader_profile_import_status.h"
#include "yuzu_video_core/shader_profile_context.h"
using namespace VideoCommon;
void require(bool value, std::source_location where = std::source_location::current()) {
    if(!value) throw std::runtime_error("cache store regression at line " + std::to_string(where.line()));
}
int main(int argc,char** argv) try {
    if(argc!=2) return 2;
    const std::filesystem::path path=argv[1];
    require(!std::filesystem::exists(path));
    std::string environment(68,'\0'); environment[0]=16; environment+=std::string(16,'A'); environment+="metadata";
    CacheStore::Record first{{environment},std::string(64,'K')};
    std::string first_id,second_id;
    CacheStore::Append(path,20,first,&first_id);
    const auto size=std::filesystem::file_size(path);
    CacheStore::Append(path,20,first); require(std::filesystem::file_size(path)==size);
    auto second=first; second.key[0]='L';auto changed_environment=environment;changed_environment.back()='B';second.environments[0]=changed_environment;
    CacheStore::Append(path,20,second,&second_id);
    const std::string context_a(64,'a'),context_b(64,'b');
    CacheStore::Append(path,20,first,nullptr,context_a);
    CacheStore::Append(path,20,second,nullptr,context_b);
    require(CacheStore::ReadContext(path,context_a)==std::set<std::string>{first_id});
    require(CacheStore::ReadContext(path,context_b)==std::set<std::string>{second_id});
    const auto store_bytes=std::filesystem::file_size(path);
    require(!CacheStore::AssociateContext(path,20,std::string(32,'?'),context_a));
    require(CacheStore::AssociateContext(path,20,first_id,context_b));
    const auto promoted_path=CacheStore::ContextIndexPath(path,context_b);
    const auto promoted_size=std::filesystem::file_size(promoted_path);
    for(unsigned i=0;i<100;++i)require(CacheStore::AssociateContext(path,20,first_id,context_b));
    require(std::filesystem::file_size(promoted_path)==promoted_size);
    require(std::filesystem::file_size(path)==store_bytes);
    require(CacheStore::ReadContext(path,context_b)==std::set<std::string>({first_id,second_id}));
    std::vector<std::thread> writers;
    for(unsigned i=0;i<8;++i)writers.emplace_back([&] {
        for(unsigned j=0;j<50;++j) {
            if(!CacheStore::AssociateContext(path,20,first_id,context_b))std::terminate();
            CacheStore::Append(path,20,second);
        }
    });
    for(auto& writer:writers)writer.join();
    require(std::filesystem::file_size(path)==store_bytes);
    require(std::filesystem::file_size(promoted_path)==promoted_size);
    auto context_path=CacheStore::ContextIndexPath(path,context_a);
    {std::ofstream tail(context_path,std::ios::binary|std::ios::app);tail<<"torn";}
    require(CacheStore::ReadContext(path,context_a).size()==1);
    CacheStore::Append(path,20,second,nullptr,context_a);
    require(CacheStore::ReadContext(path,context_a).size()==2);
    // Each version folder identifies itself from its own index header.
    require(ShaderProfile::RecordingContext(ShaderProfile::RecordingPath(path.parent_path(),context_a))==context_a);
    require(ShaderProfile::VersionContext(ShaderProfile::FindVersionDirectory(path.parent_path(),context_b))==context_b);
    require(ShaderProfile::FindVersionDirectory(path.parent_path(),std::string(64,'c')).empty());
    require(CacheStore::ReadContext(path,std::string(64,'c')).empty());
    std::set<std::string> present_ids;
    CacheStore::Read(path,20,{},&present_ids);
    JsonValue receipt;
    receipt["schema"]=1U;receipt["complete"]=true;receipt["stamp"]="identity";receipt["selection"]="selected";
    receipt["record_count"]=2U;receipt["record_ids"]=JsonValue(JsonValueType::Array);
    receipt["record_ids"].Append(ShaderProfile::LocalRecordHex(first_id));
    receipt["record_ids"].Append(ShaderProfile::LocalRecordHex(second_id));
    require(ShaderProfile::IntegratedRecordsPresent(receipt,"identity","selected",present_ids));
    require(!ShaderProfile::IntegratedRecordsPresent(receipt,"other","selected",present_ids));
    require(!ShaderProfile::IntegratedRecordsPresent(receipt,"identity","new-selection",present_ids));
    receipt["recipe_scope"]="scope";
    const auto recipe_id=std::string(64,'e');
    receipt["recipe_records"][recipe_id]=ShaderProfile::LocalRecordHex(first_id);
    require(ShaderProfile::ReusableRecipeRecords(receipt,"scope",present_ids).at(recipe_id)==first_id);
    require(ShaderProfile::ReusableRecipeRecords(receipt,"changed-scope",present_ids).empty());
    require(ShaderProfile::ReusableRecipeRecords(receipt,"scope",{}).empty());
    // A cleanly truncated cache remains valid and nonempty, but is not complete.
    std::filesystem::resize_file(path,size);
    require(CacheStore::Read(path,20,{},&present_ids).size()==1);
    require(!CacheStore::AssociateContext(path,20,second_id,context_a));
    require(CacheStore::AssociateContext(path,20,first_id,context_a));
    require(!ShaderProfile::IntegratedRecordsPresent(receipt,"identity","selected",present_ids));
    CacheStore::Append(path,20,second);
    CacheStore::Read(path,20,{},&present_ids);
    require(ShaderProfile::IntegratedRecordsPresent(receipt,"identity","selected",present_ids));
    auto invalid_receipt=receipt;invalid_receipt["record_count"]="2";
    require(!ShaderProfile::IntegratedRecordsPresent(invalid_receipt,"identity","selected",present_ids));
    invalid_receipt=receipt;invalid_receipt["record_ids"][1]=ShaderProfile::LocalRecordHex(first_id);
    require(!ShaderProfile::IntegratedRecordsPresent(invalid_receipt,"identity","selected",present_ids));
    require(!ShaderProfile::ShouldReconstructProfile(false,false,true,"selected","selected"));
    require(!ShaderProfile::ShouldReconstructProfile(false,false,false,"selected","selected"));
    require(ShaderProfile::ShouldReconstructProfile(false,false,false,"new-selection","selected"));
    require(ShaderProfile::ShouldReconstructProfile(false,true,false,"selected","selected"));
    require(ShaderProfile::ShouldReconstructProfile(true,false,false,"selected","selected"));
    require(!ShaderProfile::ShouldReconstructProfile(true,false,true,"selected","selected"));
    require(!ShaderProfile::ShouldReconstructProfile(true,true,true,"selected","selected"));
    // Failure state is local, bounded, strictly validated and never permanent.
    const auto failed_id=std::string(64,'a');
    ShaderProfile::NativeWarmupFailures failures("gpu-driver-renderer-cache-A");
    require(!failures.Defer(failed_id));
    failures.Complete(failed_id,false);
    auto persisted=failures.Save();
    ShaderProfile::NativeWarmupFailures next("gpu-driver-renderer-cache-A");
    next.Load(persisted);require(next.Defer(failed_id));
    persisted=next.Save();next.Load(persisted);require(!next.Defer(failed_id));
    next.Complete(failed_id,false);
    require(next.Defer(failed_id));require(next.Defer(failed_id));
    require(!next.Defer(failed_id));
    next.Complete(failed_id,true);require(next.Save()["entries"].size()==0);
    ShaderProfile::NativeWarmupFailures changed("gpu-driver-renderer-cache-B");
    changed.Load(failures.Save());require(!changed.Defer(failed_id));
    auto invalid=failures.Save();invalid["entries"][0]["remaining"]=99U;
    next.Load(invalid);require(!next.Defer(failed_id));
    invalid=failures.Save();invalid["entries"][0]["failures"]="1";
    next.Load(invalid);require(!next.Defer(failed_id));
    invalid=failures.Save();invalid["entries"].Append(invalid["entries"][0]);
    next.Load(invalid);require(!next.Defer(failed_id));
    auto records=CacheStore::Read(path,20);require(records.size()==2);
    require(records[0].environments==first.environments && records[1].environments==second.environments);
    require(records[0].key==first.key && records[1].key==second.key);
    bool limited=false;try{CacheStore::Read(path,20,CacheStore::ReadLimits{1024,100});}catch(const std::exception&){limited=true;}require(limited);
    limited=false;try{CacheStore::Read(path,20,CacheStore::ReadLimits{1,1024});}catch(const std::exception&){limited=true;}require(limited);
    require(CacheStore::Read(path,20).size()==2); // Budget failure never alters the source.
    // Same code, different metadata and key: one code frame, two env and two pipeline frames.
    std::ifstream file(path,std::ios::binary);std::string data((std::istreambuf_iterator<char>(file)),{});
    file.close();
    size_t cursor=16;unsigned code_count=0,env_count=0,pipeline_count=0;
    while(cursor<data.size()) {
        const auto type=static_cast<unsigned char>(data[cursor])&0x7f;size_t length{};
        for(size_t i=0;i<4;++i) length|=size_t{static_cast<unsigned char>(data[cursor+1+i])}<<(8*i);
        code_count+=type==1;env_count+=type==2;pipeline_count+=type==3;cursor+=37+length;
    }
    require(code_count==1 && env_count==2 && pipeline_count==2);
    {std::ofstream tail(path,std::ios::binary|std::ios::app);tail<<"partial";}
    require(CacheStore::Read(path,20).size()==2);
    CacheStore::Append(path,20,second);require(CacheStore::Read(path,20).size()==2);
    auto bad=path;bad+=".bad";data.back()^=1;
    {std::ofstream out(bad,std::ios::binary);out.write(data.data(),static_cast<std::streamsize>(data.size()));}
    // A corrupt frame ends the valid prefix: earlier records stay usable, the
    // corrupt one is never returned and the next append truncates and repairs.
    const auto recovered=CacheStore::Read(bad,20);require(recovered.size()<2);
    CacheStore::Append(bad,20,second);require(CacheStore::Read(bad,20).size()==recovered.size()+1);
    // A torn header or another ABI never blocks later appends.
    std::filesystem::resize_file(bad,5);require(CacheStore::Read(bad,20).empty());
    CacheStore::Append(bad,20,first);require(CacheStore::Read(bad,20).size()==1);
    bool rejected=false;try{CacheStore::Read(bad,21);}catch(const std::exception&){rejected=true;}require(rejected);
    CacheStore::Append(bad,21,first);require(CacheStore::Read(bad,21).size()==1);
    auto moved=bad;moved+=".old";require(CacheStore::Read(moved,20).size()==1);std::filesystem::remove(moved);
    {
        // Same recovery for a corrupt context association.
        CacheStore::Append(bad,21,second,nullptr,context_a);
        const auto bad_context=CacheStore::ContextIndexPath(bad,context_a);
        std::fstream io(bad_context,std::ios::binary|std::ios::in|std::ios::out);io.seekp(10);io.put('x');io.close();
        // Same size: only the write time tells the writer the file changed.
        std::filesystem::last_write_time(bad_context,std::filesystem::last_write_time(bad_context)+std::chrono::seconds(2));
        require(CacheStore::ReadContext(bad,context_a).empty());
        require(CacheStore::AssociateContext(bad,21,first_id,context_a));
        require(CacheStore::ReadContext(bad,context_a)==std::set<std::string>{first_id});
        std::filesystem::remove(bad_context);
    }
    // Switching back to an existing writer rebuilds only its ID index.
    const auto before=std::filesystem::file_size(path);CacheStore::Append(path,20,first);
    require(std::filesystem::file_size(path)==before);
    // A small file with a highly reused program must not expand once per key.
    auto dense=path;dense+=".shared";
    std::string big(68,'\0');
    const size_t code_size=1024*1024;
    for(size_t i=0;i<8;++i)big[i]=static_cast<char>(code_size>>(8*i));
    big+=std::string(code_size,'S');
    CacheStore::Record shared{{big},std::string(64,'\0')};
    for(unsigned i=0;i<300;++i) {
        for(size_t j=0;j<4;++j)shared.key[j]=static_cast<char>(i>>(8*j));
        CacheStore::Append(dense,20,shared);
    }
    auto repeated=CacheStore::Read(dense,20);
    require(repeated.size()==300 && std::filesystem::file_size(dense)<2*1024*1024);
    for(const auto& record:repeated)
        require(record.environments[0].Identity()==repeated[0].environments[0].Identity());
    for(unsigned i=300;i<375;++i) {
        auto variant=big;
        for(size_t j=0;j<4;++j)variant+=static_cast<char>(i>>(8*j));
        shared.environments[0]=std::move(variant);
        for(size_t j=0;j<4;++j)shared.key[j]=static_cast<char>(i>>(8*j));
        CacheStore::Append(dense,20,shared);
    }
    repeated=CacheStore::Read(dense,20,CacheStore::ReadLimits{2*1024*1024,1024*1024});
    require(repeated.size()==375);
    for(const auto& record:repeated)
        require(record.environments[0].ProgramIdentity()==repeated[0].environments[0].ProgramIdentity());
    // Compressed frames shrink a repetitive store and read back unchanged.
    require(std::filesystem::file_size(dense)<64*1024);
    {
        // Compact regroups appended frames into blocks: the same records in the
        // same order, with the same identities, and appends continue after them.
        const auto loose=CacheStore::Read(dense,20);
        const auto loose_size=std::filesystem::file_size(dense);
        require(!CacheStore::Compact(dense,20,loose_size+1));
        require(CacheStore::Compact(dense,20,0) && std::filesystem::file_size(dense)<loose_size);
        const auto blocks=CacheStore::Read(dense,20);
        require(blocks.size()==loose.size());
        for(size_t i=0;i<blocks.size();++i)
            require(blocks[i].id==loose[i].id && blocks[i].key==loose[i].key &&
                    blocks[i].environments[0].String()==loose[i].environments[0].String());
        require(!CacheStore::Compact(dense,20,1));
        shared.key[5]='Z';
        CacheStore::Append(dense,20,shared);
        require(CacheStore::Read(dense,20).size()==376);
        require(CacheStore::Compact(dense,20,0) && CacheStore::Read(dense,20).size()==376);
        // Only the current revision is read: a revision 2 store is foreign.
        auto blocked=path;blocked+=".blocks";
        CacheStore::Append(blocked,20,first);CacheStore::Append(blocked,20,second);
        require(CacheStore::Compact(blocked,20,0) && CacheStore::Read(blocked,20).size()==2);
        const auto set_byte=[&](u64 offset,char value) {
            std::fstream file(blocked,std::ios::binary|std::ios::in|std::ios::out);file.seekp(static_cast<std::streamoff>(offset));file.put(value);
        };
        set_byte(8,2);
        bool foreign=false;try{CacheStore::Read(blocked,20);}catch(const std::exception&){foreign=true;}
        require(foreign && !CacheStore::Compact(blocked,20,0));
        set_byte(8,3);
        // A damaged block ends the valid prefix like any frame; the writer recovers.
        set_byte(std::filesystem::file_size(blocked)-4,'!');
        require(CacheStore::Read(blocked,20).empty());
        CacheStore::Append(blocked,20,first);require(CacheStore::Read(blocked,20).size()==1);
        std::filesystem::remove(blocked);
    }
    {
        // Multiple output blocks and chunked copying of existing blocks must
        // preserve payloads, including when a temporary-file write fails.
        auto large=path;large+=".streaming";
        auto staging=large;staging+=".tmp";
        const auto bytes_of=[](const std::filesystem::path& name) {
            std::ifstream file(name,std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(file),{});
        };
        u32 random=12345;
        auto sample=first;
        std::string metadata(512*1024,'\0');
        for(unsigned i=0;i<12;++i) {
            for(auto& byte:metadata) {
                random^=random<<13;random^=random>>17;random^=random<<5;
                byte=static_cast<char>(random);
            }
            sample.environments[0]=environment+metadata;
            sample.key[0]=static_cast<char>(i);
            CacheStore::Append(large,20,sample);
        }
        const auto original=CacheStore::Read(large,20);
        require(original.size()==12);
        const auto before=bytes_of(large);
        std::filesystem::create_directory(staging);
        {std::ofstream sentinel(staging/"keep");sentinel<<"keep";}
        bool failed=false;
        try { CacheStore::Compact(large,20,0); }
        catch(const std::exception&) { failed=true; }
        require(failed && bytes_of(large)==before);
        std::filesystem::remove_all(staging);
        require(CacheStore::Compact(large,20,0));
        const auto prefix=bytes_of(large);
        require(prefix.size()>4*1024*1024);
        sample.key[0]='Z';
        CacheStore::Append(large,20,sample);
        {std::ofstream tail(large,std::ios::binary|std::ios::app);tail<<"torn";}
        require(CacheStore::Compact(large,20,0));
        const auto after=bytes_of(large);
        require(after.starts_with(prefix) && !std::filesystem::exists(staging));
        const auto records=CacheStore::Read(large,20);
        require(records.size()==13);
        for(size_t i=0;i<original.size();++i)
            require(records[i].id==original[i].id && records[i].key==original[i].key &&
                    records[i].environments[0].String()==original[i].environments[0].String());
        require(records.back().key==sample.key && records.back().environments[0].String()==sample.environments[0].String());
        std::filesystem::remove(large);
    }
    std::filesystem::remove(dense);
    std::filesystem::remove(context_path);
    context_path=CacheStore::ContextIndexPath(path,context_b);std::filesystem::remove(context_path);
    std::cout<<"code/environment/pipeline deduplication, combinations, truncated tail, integrity passed\n";
    std::filesystem::remove(path);std::filesystem::remove(bad);return 0;
} catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
