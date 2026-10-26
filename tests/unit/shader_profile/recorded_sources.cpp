// SPDX-License-Identifier: GPL-2.0-or-later
#include "yuzu_video_core/shader_profile_context.h"
#include "yuzu_video_core/shader_profile_discovery.h"
#include "yuzu_video_core/shader_profile_provenance.h"
#include "yuzu_video_core/shader_profile_publication.h"
#include "yuzu_video_core/shader_profile_versions.h"
#include "video_settings.h"
#include "yuzu_common/logging/log.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <chrono>

using namespace VideoCommon::ShaderProfile;
VideoSettings videoSettings{};
namespace Common::Log {
LogLevel g_classLevel[static_cast<uint8_t>(LogClass::Count)]{};
void FmtLogMessageImpl(LogClass, LogLevel, const char *, unsigned, const char *,
                      const char *, const fmt::format_args &) {}
}
void assert_fail_impl() { throw std::runtime_error("renderer assertion"); }
[[noreturn]] void unreachable_impl() { throw std::runtime_error("renderer unreachable"); }
namespace {
void Check(bool ok) { if (!ok) throw std::runtime_error("Recorded content selection regression"); }
void Put(std::string & bytes, size_t at, u64 value, size_t size) {
    for (size_t i=0;i<size;++i) bytes.at(at+i)=static_cast<char>(value>>(i*8));
}
std::string Nca(u64 title, char tag) {
    std::string bytes(0xc00, '\0');
    bytes.replace(0x200,4,"DNCA"); Put(bytes,0x210,title,8); bytes[0x300]=tag;
    return bytes;
}
std::string Partition(const std::vector<std::pair<std::string,std::string>> & files, bool hfs=false) {
    std::string names, payload;
    const size_t stride=hfs?64:24;
    std::string out(16+files.size()*stride,'\0'); out.replace(0,4,hfs?"HFS0":"PFS0");
    Put(out,4,files.size(),4);
    for(size_t i=0;i<files.size();++i) {
        Put(out,16+i*stride,payload.size(),8); Put(out,24+i*stride,files[i].second.size(),8);
        Put(out,32+i*stride,names.size(),4);
        names+=files[i].first; names+='\0'; payload+=files[i].second;
    }
    Put(out,8,names.size(),4); return out+names+payload;
}
void Save(const std::filesystem::path & path, std::string_view bytes) {
    std::ofstream out(path,std::ios::binary); out.write(bytes.data(),bytes.size()); Check(bool(out));
}
template<class F> void Reject(F f) { bool rejected=false; try {f();}catch(const std::exception&){rejected=true;} Check(rejected); }
}
int main(int argc,char** argv) try {
    Check(argc==2); const auto root=std::filesystem::u8path(argv[1]) /
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    const auto base=Nca(0x1000,'b'), update=Nca(0x1800,'u'), other=Nca(0x1800,'x');
    const auto base_part=ContentPart(base.size(),base), update_part=ContentPart(update.size(),update);
    Save(root/"base.dnsp",Partition({{"base.nca",base}}));
    Save(root/"newer.dnsp",Partition({{"update.nca",other}}));
    Save(root/"old-update.dnsp",Partition({{"update.nca",update}}));
    ExportOptions options; options.title_id=0x1000; options.game_root=root; options.base="base.dnsp";
    options.context=ContentContext(options.title_id,base_part,{});
    options.update="newer.dnsp"; ResolveRecordedSources(options); Check(options.update.empty());
    options.context=ContentContext(options.title_id,base_part,update_part);
    ResolveRecordedSources(options); Check(options.update=="old-update.dnsp");
    options.context=ContentContext(options.title_id,base_part,"missing");
    Reject([&]{ResolveRecordedSources(options);});
    options.context=ContentContext(options.title_id,base_part,update_part);
    options.progress=[](const char*,u64,u64){return false;};
    Reject([&]{ResolveRecordedSources(options);}); options.progress={};
    std::string card(0xf000,'\0'); card.replace(0x100,4,"DXCI"); Put(card,0x130,card.size(),8);
    card+=Partition({{"secure",Partition({{"base.nca",base},{"update.nca",update}},true)}},true);
    Save(root/"embedded.dxci",card); options.base="embedded.dxci";
    ResolveRecordedSources(options); Check(options.update==options.base);
    auto same_title_update=update;
    Put(same_title_update,0x210,0x1000,8);
    Put(same_title_update,0x240,6,4); Put(same_title_update,0x244,7,4);
    Put(same_title_update,0x508,16,8);
    Save(root/"same-title.dnsp",Partition({{"base.nca",base},{"update.nca",same_title_update}}));
    options.base="same-title.dnsp";
    options.context=ContentContext(options.title_id,base_part,ContentPart(same_title_update.size(),same_title_update));
    ResolveRecordedSources(options); Check(options.update==options.base);
    options.context=ContentContext(options.title_id,base_part,{});
    ResolveRecordedSources(options); Check(options.update.empty());
    options.context.clear(); Reject([&]{ResolveRecordedSources(options);});
    options.context=ContentContext(options.title_id,base_part,update_part);
    Put(card,0x130,UINT64_MAX,8); Save(root/"invalid.dxci",card); options.base="invalid.dxci";
    Reject([&]{ResolveRecordedSources(options);});
    options.base="../base.dnsp"; Reject([&]{ResolveRecordedSources(options);});
    const auto disabled=ContentProvenance(0x1000,base_part,{},"1.0.0","9.9.9",true,false);
    const auto enabled=ContentProvenance(0x1000,base_part,update_part,"1.0.0","1.3.0",true,true);
    Check(disabled["context"].asString()!=enabled["context"].asString());
    Check(disabled["update_version"].asString().empty());
    SaveProvenance(root,disabled); SaveProvenance(root,enabled);
    Save(RecordingPath(root,disabled["context"].asString()),"recording");
    Save(RecordingPath(root,enabled["context"].asString()),"recording");
    auto versions=ListRecordedVersions(root,0x1000,"Game: English");
    Check(versions.entries.size()==2 && versions.unavailable==0);
    Check(ReadableProfileFilename("Game: English",enabled,0x1000)==
        "Game- English - Update 1.3.0 [0000000000001000].nxshader");
    // One folder per version, named after it.
    Check(versions.entries[0].short_label=="Base 1.0.0" && versions.entries[1].short_label=="Update 1.3.0");
    Check(versions.entries[1].directory.filename().string()=="Update 1.3.0 ("+enabled["context"].asString().substr(0,12)+")");
    Check(RecordingContext(versions.entries[1].recording)==enabled["context"].asString());
    Check(CombinedProfileFilename("Game",{&versions.entries[0],&versions.entries[1]},0x1000)==
        "Game - Base 1.0.0 + Update 1.3.0 [0000000000001000].nxshader");
    Check(VersionLess("Update 1.9.0","Update 1.10.0") && !VersionLess("Update 1.10.0","Update 1.9.0"));
    Check(ListRecordedVersions(root,0x2000,"Game").entries.empty());
    const auto saved=RecordedProvenance(root,disabled["context"].asString(),0x1000);
    Check(!saved["update_enabled"].asBool());
    Check(RecordedVersionLabel(saved)=="Base 1.0.0; update disabled");
    Check(RecordedVersionLabel(RecordedProvenance(root,enabled["context"].asString(),0x1000))=="Base 1.0.0; update 1.3.0 enabled");
    Check(RecordedVersionLabel(RecordedProvenance(root,ContentContext(0x1000,base_part,"uncaptured"),0x1000))=="Recorded version metadata unavailable");
    Reject([&]{RecordedProvenance(root,enabled["context"].asString(),0x2000);});
    auto tampered=enabled;tampered["update_program"]="different"; SaveProvenance(root,tampered);
    versions=ListRecordedVersions(root,0x1000,"Game");
    Check(versions.entries.size()==1 && versions.unavailable==1);
    Reject([&]{RecordedProvenance(root,enabled["context"].asString(),0x1000);});
    Reject([&]{ContentProvenance(0x1000,base_part,update_part,"1.0.0","1.3.0",true,false);});
    Reject([&]{ContentProvenance(0x1000,base_part,update_part,"1.0.0","1.3.0",false,true);});
    Save(root/"published.nxshader","previous complete profile");
    Reject([&]{ReplaceCompletedExport(root/"missing.partial",root/"published.nxshader");});
    { std::ifstream file(root/"published.nxshader",std::ios::binary); Check(std::string((std::istreambuf_iterator<char>(file)),{})=="previous complete profile"); }
    Save(root/"complete.partial","new complete profile");
    ReplaceCompletedExport(root/"complete.partial",root/"published.nxshader");
    Check(!std::filesystem::exists(root/"complete.partial"));
    { std::ifstream file(root/"published.nxshader",std::ios::binary); Check(std::string((std::istreambuf_iterator<char>(file)),{})=="new complete profile"); }
    std::cout<<"PASS: exact base/update selection, embedded DXCI, cancellation and invalid inputs\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
