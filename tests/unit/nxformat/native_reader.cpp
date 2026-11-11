// Standalone loader/VFS integration check; no CPU execution is claimed here.
#include "core/loader/nx.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/vfs/vfs_nx.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "nxformat/nx_disk_source.hpp"
#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>
#include <chrono>
#include <functional>
#include "core/file_sys/control_metadata.h"
#include <cstdlib>

namespace {
class Input final : public FileSys::VfsFile {
public:
    explicit Input(const std::filesystem::path& path):name_(path.filename().string()) {
        auto opened=nx::DiskSource::open(path); if (opened) source_=*opened.value;
    }
    std::string GetName() const override {return name_;}
    size_t GetSize() const override {return source_ ? static_cast<size_t>(source_->size()) : 0;}
    FileSys::VirtualDir GetContainingDirectory() const override {return nullptr;}
    bool IsWritable() const override {return false;}
    bool IsReadable() const override {return source_!=nullptr;}
    bool Resize(size_t) override {return false;}
    bool Rename(std::string_view) override {return false;}
    size_t Write(const u8*,size_t,size_t) override {return 0;}
    size_t Read(u8* output,size_t length,size_t offset) const override {
        if (!source_ || offset>source_->size()) return 0;
        length=static_cast<size_t>(std::min<uint64_t>(length,source_->size()-offset));
        return source_->read_at(offset,output,length) ? length : 0;
    }
private:
    std::string name_; std::shared_ptr<nx::DiskSource> source_;
};
// Defaults only: language index 1 (American English) for title/NACP lookups.
class StubSettings final : public IModuleSettings {
public:
    const char* GetString(const char*) const override { return ""; }
    bool GetBool(const char*) const override { return false; }
    int32_t GetInt(const char*) const override { return 1; }
    float GetFloat(const char*) const override { return 0.0f; }
    void SetString(const char*,const char*) override {}
    void SetBool(const char*,bool) override {}
    void SetInt(const char*,int32_t) override {}
    void SetFloat(const char*,float) override {}
    void SetDefaultBool(const char*,bool) override {}
    void SetDefaultInt(const char*,int) override {}
    void SetDefaultFloat(const char*,float) override {}
    void SetDefaultString(const char*,const char*) override {}
    const char* GetSectionSettings(const char*) const override { return "{}"; }
    void SetSectionSettings(const char*,const char*) override {}
    void RegisterCallback(const char*,SettingChangeCallback,void*) override {}
    void UnregisterCallback(const char*,SettingChangeCallback,void*) override {}
    void SetDebugSetting(const char*,bool) override {}
    bool DebugSettingsEnabled() const override { return false; }
    const char* GetGameSectionSettings(const char*) const override { return "{}"; }
    void SetGameSectionSettings(const char*,const char*) override {}
};
class Provider final : public IManualContentProvider {
public:
    unsigned dlc=0;
    void AddEntry(LoaderTitleType type,LoaderContentRecordType,uint64_t,uint32_t,IVirtualFile* file) override {
        if (type==LoaderTitleType::AOC) ++dlc;
        file->Release();
    }
    void ClearAllEntries() override {}
};
}
extern IModuleSettings* g_settings;
int main(int argc,char** argv) {
    static StubSettings settings; g_settings=&settings;
    if (argc<2) return 2;
    if (std::string_view(argv[1])=="--decoder-recovery") {
        if (argc!=3) return 2;
        const auto path=std::filesystem::u8path(argv[2]);
        auto input=std::make_shared<Input>(path);
        if (input->GetSize()>1024*1024) return 2; // Small artificial runtime_cases fixture only.
        std::vector<u8> bytes(input->GetSize());
        if (input->Read(bytes.data(),bytes.size(),0)!=bytes.size()) return 1;
        auto u64=[&](size_t at) {
            uint64_t value=0;
            for (unsigned i=0;i<8;++i) value|=uint64_t(bytes.at(at+i))<<(i*8);
            return value;
        };
        const auto table=static_cast<size_t>(u64(88));
        const auto payload=static_cast<size_t>(u64(table));
        // Fixed redistributable fixture: a 26-byte frame for 256 KiB of 0x01.
        const std::array<u8,26> frame{0x28,0xb5,0x2f,0xfd,0xa0,0,0,4,0,0x54,0,0,
            0x10,1,1,1,0,0xfb,0xff,0x39,0xc0,2,3,0,0x10,1};
        if (payload>bytes.size() || frame.size()>bytes.size()-payload ||
            !std::equal(frame.begin(),frame.end(),bytes.begin()+payload)) return 2;
        class ReadOnlyBytes final : public FileSys::VectorVfsFile {
        public:
            explicit ReadOnlyBytes(std::vector<u8> value):VectorVfsFile(std::move(value)) {}
            bool IsWritable() const override { return false; }
        };
        nx::SessionOptions options;
        options.sequential_prefetch=false;
        options.reader.max_cached_block_bytes=0;
        options.reader.max_cached_blocks=0; // Every valid read must use the thread's decoder.
        auto mount=[&](std::vector<u8> value) {
            return FileSys::OpenNxSession(std::make_shared<ReadOnlyBytes>(std::move(value)),{},options);
        };
        auto broken=bytes;
        broken[payload+12]=0; // Valid frame extent/size; invalid compressed literals.
        auto malformed=mount(std::move(broken));
        broken=bytes;
        broken[payload+10]=0xff; // Frame block extends beyond the supplied stored bytes.
        auto truncated=mount(std::move(broken));
        auto good=mount(bytes);
        if (!good || !malformed || !truncated) return 1;
        bytes.resize(20);
        if (mount(std::move(bytes))) return 1;
        auto file=(*good.value)->archive().resolve_file(0,"data.bin");
        if (!file) return 1;
        std::atomic<unsigned> errors{0};
        auto exercise=[&] {
            for (unsigned i=0;i<64;++i) {
                for (auto bad:{*malformed.value,*truncated.value}) {
                    std::array<u8,4096> output; output.fill(0xee);
                    auto result=bad->read(file.value->id,0,output.size(),output.data(),output.size());
                    if (result || result.error.code!=nx::ErrorCode::decompression ||
                        !std::all_of(output.begin(),output.end(),[](u8 v){return v==0xee;})) ++errors;
                    result=(*good.value)->read(file.value->id,0,output.size(),output.data(),output.size());
                    if (!result || !std::all_of(output.begin(),output.end(),[](u8 v){return v==1;})) ++errors;
                }
            }
        };
        exercise(); // Decode error followed by a valid frame on the very same thread.
        for (unsigned batch=0;batch<4;++batch) {
            std::vector<std::thread> workers;
            for (unsigned i=0;i<8;++i) workers.emplace_back(exercise);
            for (auto& worker:workers) worker.join();
        }
        if (errors) return 1;
        std::cout<<"PASS actual NXEmu decoder: malformed/truncated frames, atomic failure, same-thread recovery, concurrent contexts and repeated worker exit\n";
        return 0;
    }
    if (std::string_view(argv[1])=="--native-bytes") {
        if (argc!=4) return 2;
        const auto path=std::filesystem::u8path(argv[2]);
        auto opened=FileSys::OpenNxSession(std::make_shared<Input>(path),path.string()); if (!opened) return 1;
        std::ifstream expected(std::filesystem::u8path(argv[3])); if (!expected) return 2;
        auto session=*opened.value; std::string line;
        std::vector<uint8_t> native(256*1024),packed(native.size()); unsigned files=0; uint64_t bytes=0;
        const auto started=std::chrono::steady_clock::now();
        while (std::getline(expected,line)) {
            std::istringstream row(line); std::string snapshot,name,container,offset,length;
            if (!std::getline(row,snapshot,'\t') || !std::getline(row,name,'\t') || !std::getline(row,container,'\t') ||
                !std::getline(row,offset,'\t') || !std::getline(row,length)) return 2;
            auto root=FileSys::NxSnapshotDirectory(session,static_cast<uint32_t>(std::stoul(snapshot)));
            auto file=root ? root->GetFile(name) : nullptr;
            const auto size=std::stoull(length), start=std::stoull(offset);
            auto source=nx::DiskSource::open(std::filesystem::u8path(container));
            if (!file || file->GetSize()!=size || !source) return 1;
            for (uint64_t at=0;at<size;) {
                const auto count=static_cast<size_t>(std::min<uint64_t>(native.size(),size-at));
                if (!(*source.value)->read_at(start+at,native.data(),count) || file->Read(packed.data(),count,at)!=count ||
                    !std::equal(native.begin(),native.begin()+count,packed.begin())) return 1;
                at+=count; bytes+=count;
            }
            ++files;
        }
        if (!files) return 1;
        const auto stats=session->stats();
        std::cout<<"PASS actual NXEmu VFS byte comparison: files="<<files<<" bytes="<<bytes
            <<" seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()
            <<" hits="<<stats.cache.hits<<" misses="<<stats.cache.misses<<" decodes="<<stats.cache.decompressions
            <<" decode_ns="<<stats.cache.decompression_ns<<" cache_peak="<<stats.cache.peak_bytes<<'\n';
        return 0;
    }
    if (std::string_view(argv[1])=="--matrix") {
        if (argc!=3) return 2;
        const auto path=std::filesystem::u8path(argv[2]); auto file=std::make_shared<Input>(path);
        auto opened=FileSys::OpenNxSession(file,path.string()); if (!opened) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
        const auto& archive=(*opened.value)->archive();
        if (std::getenv("NX_TEST_DIAGNOSTICS")) {
            auto root=FileSys::NxSnapshotDirectory(*opened.value,0);
            for (auto content:root->GetFiles()) {
                FileSys::NCA nca(content);
                std::cerr<<content->GetName()<<" status="<<unsigned(nca.GetStatus())<<" dirs="<<nca.GetSubdirectories().size()<<'\n';
                for (auto dir:nca.GetSubdirectories()) for (auto child:dir->GetFiles()) std::cerr<<"  "<<child->GetName()<<" size="<<child->GetSize()<<'\n';
            }
        }
        constexpr uint64_t application=0x0100000000010000;
        constexpr uint64_t auxiliary_program=application+0x123;
        const auto& display=archive.snapshots()[archive.applications()[0].metadata_snapshot];
        if (archive.programs()[display.program_index].id!=auxiliary_program || display.numeric_version!=1) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
        // EmulatorSession::LoadProgram supplies a nonzero base ID plus the
        // requested native index. The secondary ID is deliberately not base+7.
        for (const uint64_t requested : {uint64_t{0}, application}) {
            for (unsigned offset=0;offset<2;++offset) {
                Loader::AppLoader_NX latest(file,requested,offset ? 7 : 0,path.string());
                if (latest.GetStatus()!=LoaderResultStatus::Success) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
                const auto& s=latest.Session()->archive().snapshots()[latest.SelectedSnapshot()];
                uint64_t id=0; if (latest.ReadProgramId(id)!=LoaderResultStatus::Success || id!=(offset ? auxiliary_program : application) || s.numeric_version!=3) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            }
        }
        for (const size_t index : {size_t{1}, size_t{256}}) {
            Loader::AppLoader_NX invalid(file,application,index,path.string());
            if (invalid.GetStatus()==LoaderResultStatus::Success) return 1;
        }
        Loader::AppLoader_NX unknown(file,application+1,7,path.string());
        if (unknown.GetStatus()==LoaderResultStatus::Success) return 1;
        unsigned checked=0;
        for (const auto& snapshot:archive.snapshots()) {
            if (snapshot.type==2) continue;
            const auto id=archive.programs()[snapshot.program_index].id;
            const auto auxiliary=id==auxiliary_program;
            const auto version=snapshot.numeric_version;
            const uint8_t expected=version==1 && auxiliary ? 21 : version==2 ? (auxiliary ? 99 : 22) : auxiliary ? 7 : 1;
            Loader::AppLoader_NX loader(file,id,0,path.string(),snapshot.identity);
            if (loader.GetStatus()!=LoaderResultStatus::Success) { std::cerr<<"matrix rejected version "<<version<<" auxiliary "<<auxiliary<<'\n'; { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; } }
            uint64_t actual=0; loader.ReadProgramId(actual); if (actual!=id) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            if (loader.SelectedVersion()!=version || loader.UpdateSelected()!=(snapshot.type==1)) return 1;
            std::array<char,128> title{}; uint32_t title_size=static_cast<uint32_t>(title.size());
            if (loader.ReadTitle(title.data(),&title_size)!=LoaderResultStatus::Success ||
                std::string_view(title.data(),title_size)!="Auxiliary display metadata") return 1;
            std::array<char,32> text{}; uint32_t text_size=static_cast<uint32_t>(text.size());
            const auto display_status=loader.ReadDisplayVersion(text.data(),&text_size);
            const bool own_version=(!auxiliary && version==0) || (auxiliary && version==1);
            const bool fallback_version=snapshot.type==1 || snapshot.type==3;
            if ((display_status==LoaderResultStatus::Success)!=(own_version || fallback_version)) return 1;
            if (!own_version && fallback_version &&
                std::string_view(text.data(),text_size)!="v"+std::to_string(version)) return 1;
            std::array<uint8_t,96> header{}; auto main=loader.ExeFS()->GetFile("main");
            if (!main || main->Read(header.data(),header.size(),0)!=header.size()) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            for (unsigned i=64;i<96;++i) if (header[i]!=expected || snapshot.build_id[i-64]!=expected) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            if (snapshot.type==0 && !auxiliary) {
                auto bytes=loader.ExeFS()->GetFile("main.npdm")->ReadAllBytes();
                bytes[0xc4]=0x78; bytes[0xc5]=0x56; bytes[0xc6]=0x34; bytes[0xc7]=0x12;
                FileSys::ProgramMetadata metadata;
                if (metadata.Load(std::make_shared<FileSys::VectorVfsFile>(bytes))!=LoaderResultStatus::Success ||
                    metadata.GetFilesystemPermissions()!=0x12345678) return 1;
                std::fill(bytes.begin()+0xb4,bytes.begin()+0xb8,0xff);
                if (metadata.Load(std::make_shared<FileSys::VectorVfsFile>(bytes))==LoaderResultStatus::Success) return 1;
            }
            FileSys::ManualContentProvider fallback;
            fallback.AddEntry(LoaderTitleType::Application,LoaderContentRecordType::Program,id,999,file);
            fallback.AddEntry(LoaderTitleType::AOC,LoaderContentRecordType::Data,application+0x1002,999,file);
            auto& provider=loader.ContentProvider(fallback);
            if (!auxiliary) {
                const auto origin=loader.ControlSnapshot();
                if (origin>=archive.snapshots().size() || archive.snapshots()[origin].type!=0) return 1;
                auto control=provider.GetEntryNCA(id,LoaderContentRecordType::Control);
                auto control_root=control ? FileSys::ExtractRomFS(control->RomFS()) : nullptr;
                auto nacp=control_root ? control_root->GetFile("control.nacp") : nullptr;
                std::array<uint8_t,15> native_title{};
                if (!nacp || nacp->Read(native_title.data(),native_title.size(),0)!=native_title.size() ||
                    std::string_view(reinterpret_cast<const char*>(native_title.data()),native_title.size())!="Synthetic Title") return 1;
            } else if (version==1 && loader.ControlSnapshot()!=loader.SelectedSnapshot()) return 1;
            auto nca=provider.GetEntryNCA(id,LoaderContentRecordType::Program);
            if (!nca || !nca->GetExeFS()) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            auto active_main=nca->GetExeFS()->GetFile("main");
            if (!active_main || active_main->Read(header.data(),header.size(),0)!=header.size() || header[64]!=expected) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            const auto dlcs=provider.ListEntriesFilter(LoaderTitleType::AOC,LoaderContentRecordType::Data,{});
            if (dlcs.size()!=(version<2 ? 1u : 2u)) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            if (version<2 && provider.GetEntryRaw(application+0x1002,LoaderContentRecordType::Data)) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            for (auto dlc:dlcs) if (!provider.GetEntryNCA(dlc.titleID,dlc.type)) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            if (version==3) {
                // A newly declared native Data NCA is exposed under its actual
                // native owner. No title-ID bit manipulation creates an alias.
                auto data=provider.GetEntryNCA(application|0x800,LoaderContentRecordType::Data);
                if (!data) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
                auto romfs=FileSys::ExtractRomFS(data->RomFS());
                auto asset=romfs ? romfs->GetFile("dlc.bin") : nullptr;
                std::vector<uint8_t> bytes(300*1024);
                if (!asset || asset->Read(bytes.data(),bytes.size(),200*1024)!=bytes.size() ||
                    !std::all_of(bytes.begin(),bytes.end(),[](auto b){return b==5;})) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            }
            Loader::AppLoader_NX wrong(file,auxiliary ? application : auxiliary_program,0,path.string(),snapshot.identity);
            if (wrong.GetStatus()==LoaderResultStatus::Success) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
            ++checked;
        }
        if (checked!=8) { std::cerr<<"matrix failure line "<<__LINE__<<'\n'; return 1; }
        auto missing_path=path.parent_path()/"missing-base.nx";
        if (!std::filesystem::is_regular_file(missing_path)) return 1;
        auto missing_session=FileSys::OpenNxSession(std::make_shared<Input>(missing_path),missing_path.string());
        if (!missing_session) return 1; // valid NX structure, missing native dependency
        Loader::AppLoader_NX missing(std::make_shared<Input>(missing_path),0,0,missing_path.string());
        if (missing.GetStatus()==LoaderResultStatus::Success) return 1;
        Loader::AppLoader_NX application_mismatch(file,0,0,path.string(),{}, {}, application+1);
        if (application_mismatch.GetStatus()==LoaderResultStatus::Success) return 1;
        std::cout<<"PASS frontend base-ID/index selection and 8 native multi-Program versions: exact IDs, one/both replacements, data-only Base inheritance, independent display selection, DLC requirements, native provider precedence and cross-block Data reads (no execution)\n";
        return 0;
    }
    if (std::string_view(argv[1])=="--vfs") {
        if (argc!=3) return 2;
        const auto path=std::filesystem::u8path(argv[2]);
        auto input=std::make_shared<Input>(path);
        auto opened=FileSys::OpenNxSession(input,path.string());
        if (!opened) return 1;
        auto session=*opened.value;
        auto root=FileSys::NxSnapshotDirectory(session,0);
        auto data=root->GetFile("data.bin");
        auto mirror=FileSys::NxSnapshotDirectory(session,1)->GetFile("mirror.bin");
        if (!data || !mirror || root->GetFile("missing") || data->IsWritable() || data->Resize(0) ||
            data->Write(nullptr,0,0) || root->CreateFileRelative("forbidden")) return 1;
        // File handles keep the pinned archive alive after the caller releases
        // the original input/session/root. No path is reopened by these reads.
        root.reset(); input.reset(); opened.value.reset(); session.reset();
        std::atomic<unsigned> errors{0}; std::vector<std::thread> workers;
        for (unsigned t=0;t<8;++t) workers.emplace_back([&,t] {
            auto file=t%2 ? data : mirror;
            std::array<uint8_t,4096> bytes{};
            for (unsigned n=0;n<100;++n) {
                const auto at=((n*7+t)%16)*256*1024ull+100*1024;
                if (file->Read(bytes.data(),bytes.size(),at)!=bytes.size()) { ++errors; continue; }
                for (auto b:bytes) if (b!=at/(256*1024)+1) { ++errors; break; }
            }
        });
        for (auto& worker:workers) worker.join();
        std::array<uint8_t,32> eof{}; eof.fill(0xee);
        if (data->Read(eof.data(),32,data->GetSize()-7)!=7 || eof[7]!=0xee ||
            data->Read(eof.data(),1,data->GetSize()+1)!=0 || errors) return 1;
        std::cout<<"PASS actual NXEmu VFS: lifetime, concurrent bytes, snapshot sharing, read-only and EOF\n";
        return 0;
    }
    const auto path=std::filesystem::u8path(argv[1]);
    auto file=std::make_shared<Input>(path);
    if (Loader::AppLoader_NX::IdentifyType(file)!=LoaderFileType::NX) return 3;
    const uint64_t program=argc>2 ? std::stoull(argv[2],nullptr,16) : 0;
    const size_t offset=argc>3 ? std::stoull(argv[3]) : 0;
    std::optional<std::array<uint8_t,32>> identity;
    if (argc>4) {
        std::string text=argv[4]; if (text.size()!=64) return 2;
        identity.emplace(); for (size_t i=0;i<32;++i) (*identity)[i]=static_cast<uint8_t>(std::stoul(text.substr(i*2,2),nullptr,16));
    }
    Loader::AppLoader_NX loader(file,program,offset,path.string(),identity);
    std::cout<<"status="<<unsigned(loader.GetStatus())<<" snapshot="<<loader.SelectedSnapshot()<<'\n';
    if (loader.GetStatus()!=LoaderResultStatus::Success) {
        auto exefs=loader.ExeFS();
        if (exefs) {
            auto npdm=exefs->GetFile("main.npdm");
            if (npdm) { FileSys::ProgramMetadata metadata; const auto result=metadata.Load(npdm);
                std::cout<<"NPDM size="<<npdm->GetSize()<<" parse="<<unsigned(result)<<" id="<<std::hex<<metadata.GetTitleID()<<std::dec<<'\n'; }
        }
        return 1;
    }
    uint64_t selected=0; loader.ReadProgramId(selected);
    uint32_t count=0; loader.ReadProgramIds(nullptr,&count);
    auto main=loader.ExeFS()->GetFile("main");
    std::array<uint8_t,96> header{};
    if (!main || main->Read(header.data(),header.size(),0)!=header.size()) return 1;
    std::cout<<"program="<<std::hex<<selected<<std::dec<<" programs="<<count<<" build=";
    static constexpr char Hex[]="0123456789abcdef";
    for (unsigned i=64;i<96;++i) std::cout<<Hex[header[i]>>4]<<Hex[header[i]&15];
    std::cout<<'\n';
    FileSys::VirtualFile romfs;
    const auto romfs_status=loader.ReadRomFS(romfs);
    if (romfs_status==LoaderResultStatus::Success && romfs->GetSize()) {
        std::array<uint8_t,32> bytes{}; if (romfs->Read(bytes.data(),bytes.size(),0)!=bytes.size()) return 1;
        // Walk the RomFS as the game sees it and read every file once.
        auto root=FileSys::ExtractRomFS(romfs);
        if (!root) { std::cerr<<"RomFS does not parse\n"; return 1; }
        uint64_t files=0,bytes_read=0; std::vector<u8> buffer;
        std::function<bool(const FileSys::VirtualDir&)> walk=[&](const FileSys::VirtualDir& dir) {
            for (const auto& f:dir->GetFiles()) {
                buffer.resize(f->GetSize());
                if (f->Read(buffer.data(),buffer.size(),0)!=buffer.size()) return false;
                ++files; bytes_read+=buffer.size();
            }
            for (const auto& d:dir->GetSubdirectories()) if (!walk(d)) return false;
            return true;
        };
        if (!walk(root)) { std::cerr<<"RomFS file read failed\n"; return 1; }
        std::cout<<"romfs_size="<<romfs->GetSize()<<" romfs_files="<<files<<" romfs_bytes="<<bytes_read<<'\n';
    } else std::cout<<"romfs status="<<unsigned(romfs_status)<<'\n';
    std::array<char,512> title{}; uint32_t title_size=title.size();
    if (loader.ReadTitle(title.data(),&title_size)==LoaderResultStatus::Success) std::cout<<"title="<<std::string(title.data(),title_size)<<'\n';
    FileSys::NACP nacp;
    if (loader.ReadControlData(nacp)==LoaderResultStatus::Success) std::cout<<"nacp_name="<<nacp.GetApplicationName()<<" nacp_version="<<nacp.GetVersionString()<<'\n';
    else std::cout<<"no control data\n";
    uint32_t icon_size=0; if (loader.ReadIcon(nullptr,&icon_size)==LoaderResultStatus::Success) std::cout<<"icon_bytes="<<icon_size<<'\n';
    Provider provider; loader.AddToManualContentProvider(provider);
    const auto stats=loader.Session()->stats();
    std::cout<<"PASS NXEmu native loader/VFS acceptance; DLC records="<<provider.dlc
        <<" cache_hits="<<stats.cache.hits<<" decodes="<<stats.cache.decompressions
        <<" cache_bytes="<<stats.cache.bytes<<" (no execution claim)\n";
}
