#include "nx.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/filesystem.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/file_sys/vfs/vfs_nx.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "system_loader.h"
#include "loader_settings.h"
#include "yuzu_common/hex_util.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace Loader {
namespace {
constexpr uint32_t Absent=0xffffffffu;
// NX stores lowercase BCP 47 tags; this table follows FileSys::LANGUAGE_NAMES.
constexpr std::array<std::pair<const char*,const char*>,16> NxLanguageTags{{
    {"AmericanEnglish","en-us"},{"BritishEnglish","en-gb"},{"Japanese","ja"},{"French","fr"},
    {"German","de"},{"LatinAmericanSpanish","es-419"},{"Spanish","es"},{"Italian","it"},
    {"Dutch","nl"},{"CanadianFrench","fr-ca"},{"Portuguese","pt"},{"Russian","ru"},
    {"Korean","ko"},{"TraditionalChinese","zh-hant"},{"SimplifiedChinese","zh-hans"},
    {"BrazilianPortuguese","pt-br"},
}};
// First localized entry, in the configured language order, that has what
// `usable` asks for (a title or an icon). Absent means use the default.
template <class Usable>
uint32_t PreferredLocalized(const nx::Archive& archive,uint32_t app,Usable usable) {
    const auto& a=archive.applications()[app];
    for (const char* native:FileSys::GetApplicationLanguagePriorityNames()) {
        const auto tag=std::find_if(NxLanguageTags.begin(),NxLanguageTags.end(),[&](const auto& t) { return std::strcmp(t.first,native)==0; });
        if (tag==NxLanguageTags.end()) continue;
        for (uint32_t i=a.first_localized;i<a.first_localized+a.localized_count;++i)
            if (archive.localized_language(i)==tag->second && usable(i)) return i;
    }
    return Absent;
}
struct NativeMeta {
    FileSys::VirtualDir root;
    std::unique_ptr<FileSys::CNMT> cnmt;
    uint64_t application=0;
    uint64_t related=0;
    uint32_t minimum=0;
};
uint64_t U64(const uint8_t* p) { uint64_t v=0; for (unsigned i=0;i<8;++i) v|=uint64_t(p[i])<<(8*i); return v; }
uint32_t U32(const uint8_t* p) { return uint32_t(U64(p)&0xffffffffu); }
FileSys::VirtualFile Content(const NativeMeta& meta,const FileSys::ContentRecord& record) {
    return meta.root->GetFile(Common::HexToString(record.nca_id,false)+".nca");
}
bool Header(const FileSys::VirtualFile& file,uint8_t type,uint64_t* program=nullptr) {
    std::array<uint8_t,32> h{};
    if (!file || file->Read(h.data(),h.size(),0x200)!=h.size() || std::memcmp(h.data(),"DNCA",4) ||
        h[5]!=type || U64(h.data()+8)!=file->GetSize()) return false;
    if (program) *program=U64(h.data()+16);
    return true;
}
bool ReadMeta(const std::shared_ptr<nx::Session>& session,uint32_t index,uint64_t application,NativeMeta& meta) {
    const auto& snapshot=session->archive().snapshots()[index];
    meta.root=FileSys::NxSnapshotDirectory(session,index);
    if (!meta.root) return false;
    auto file=meta.root->GetFile(Common::HexToString(snapshot.content_id,false)+".cnmt.nca");
    if (!Header(file,1)) return false;
    FileSys::NCA nca(file);
    if (nca.GetStatus()!=LoaderResultStatus::Success) return false;
    FileSys::VirtualFile cnmt_file;
    for (auto& dir:nca.GetSubdirectories()) for (auto& entry:dir->GetFiles()) {
        if (entry->GetExtension()=="cnmt") { if (cnmt_file) return false; cnmt_file=entry; }
    }
    if (!cnmt_file || cnmt_file->GetSize()<48 || cnmt_file->GetSize()>4*1024*1024) return false;
    std::array<uint8_t,56> h{};
    if (cnmt_file->Read(h.data(),48,0)!=48 || h[13]!=0) return false;
    const auto type=h[12];
    if (type!=(snapshot.type==0 ? 0x80 : snapshot.type==1 ? 0x81 : 0x82) || U32(h.data()+8)!=snapshot.numeric_version) return false;
    const auto extended=uint16_t(h[14]|uint16_t(h[15])<<8);
    const auto count=uint16_t(h[16]|uint16_t(h[17])<<8);
    const auto metas=uint16_t(h[18]|uint16_t(h[19])<<8);
    if (extended<(type==0x81 ? 24 : 16) || count>4096 ||
        32ull+extended+uint64_t(count)*56+uint64_t(metas)*16>cnmt_file->GetSize()) return false;
    meta.application=type==0x80 ? U64(h.data()) : U64(h.data()+32);
    meta.related=U64(h.data()+32);
    if (meta.application!=application) return false;
    if (type==0x82) meta.minimum=U32(h.data()+40);
    meta.cnmt=std::make_unique<FileSys::CNMT>(cnmt_file);
    if (meta.cnmt->GetContentRecords().size()!=count) return false;
    std::set<std::array<uint8_t,16>> ids;
    std::set<uint8_t> program_offsets;
    std::set<uint64_t> program_ids;
    std::set<std::pair<uint64_t,uint8_t>> controls;
    for (const auto& record:meta.cnmt->GetContentRecords()) {
        if (!ids.insert(record.nca_id).second) return false;
        auto content=Content(meta,record);
        if (!content && record.type==LoaderContentRecordType::DeltaFragment) continue;
        if (!content) return false;
        if (record.type==LoaderContentRecordType::Program || record.type==LoaderContentRecordType::Control) {
            uint64_t owner=0;
            if (!Header(content,record.type==LoaderContentRecordType::Program ? 0 : 2,&owner)) return false;
            if (record.type==LoaderContentRecordType::Program) {
                if (type==0x82 || !program_offsets.insert(record.id_offset).second || !program_ids.insert(owner).second) return false;
            } else if (!controls.emplace(owner,record.id_offset).second) return false;
        }
    }
    return true;
}
FileSys::VirtualFile Find(const NativeMeta& meta,LoaderContentRecordType type,uint64_t id,uint8_t* offset=nullptr,std::optional<uint8_t> expected={}) {
    FileSys::VirtualFile result;
    for (const auto& record:meta.cnmt->GetContentRecords()) {
        if (record.type!=type || (expected && record.id_offset!=*expected)) continue;
        auto file=Content(meta,record); uint64_t owner=0;
        if (!Header(file,type==LoaderContentRecordType::Program ? 0 : 2,&owner)) continue;
        if (owner!=id) continue;
        if (result) return nullptr;
        result=std::move(file); if (offset) *offset=record.id_offset;
    }
    return result;
}
LoaderResultStatus Copy(const uint8_t* data,size_t count,uint8_t* output,uint32_t* size) {
    if (!size || count>0xffffffffu) return LoaderResultStatus::ErrorNotImplemented;
    if (output && *size<count) return LoaderResultStatus::ErrorBufferTooSmall;
    *size=static_cast<uint32_t>(count);
    if (output && count) std::memcpy(output,data,count);
    return LoaderResultStatus::Success;
}
// An exact selected native view over the existing ContentProvider interface.
// NCA/BKTR interpretation remains entirely in the existing native NCA reader.
class SelectedContent final : public FileSys::ContentProvider {
public:
    struct Entry {
        LoaderTitleType title_type;
        uint32_t version;
        FileSys::VirtualFile file,base;
    };
    using Key=std::pair<uint64_t,LoaderContentRecordType>;
    std::map<Key,Entry> entries;
    std::set<uint64_t> owned;
    FileSys::ContentProvider* fallback=nullptr;
    std::once_flag fallback_ready;
    void Refresh() override {}
    bool OwnsTitle(uint64_t id) const override { return owned.count(id)!=0; }
    bool HasEntry(uint64_t id,LoaderContentRecordType type) const override {
        return entries.count({id,type}) || (!owned.count(id) && fallback && fallback->HasEntry(id,type));
    }
    std::optional<uint32_t> GetEntryVersion(uint64_t id) const override {
        std::optional<uint32_t> result;
        for (const auto& [key,entry]:entries) if (key.first==id) result=entry.version;
        return result ? result : !owned.count(id) && fallback ? fallback->GetEntryVersion(id) : std::nullopt;
    }
    FileSys::VirtualFile GetEntryRaw(uint64_t id,LoaderContentRecordType type) const override {
        auto it=entries.find({id,type});
        return it!=entries.end() ? it->second.file : !owned.count(id) && fallback ? fallback->GetEntryRaw(id,type) : nullptr;
    }
    FileSys::VirtualFile GetEntryUnparsed(uint64_t id,LoaderContentRecordType type) const override { return GetEntryRaw(id,type); }
    std::unique_ptr<FileSys::NCA> GetEntryNCA(uint64_t id,LoaderContentRecordType type) const override {
        auto it=entries.find({id,type});
        if (it==entries.end()) return !owned.count(id) && fallback ? fallback->GetEntryNCA(id,type) : nullptr;
        const auto& e=it->second;
        auto base=e.base ? std::make_unique<FileSys::NCA>(e.base) : nullptr;
        if (base && base->GetStatus()!=LoaderResultStatus::Success) return nullptr;
        auto nca=std::make_unique<FileSys::NCA>(e.file,base.get());
        return nca->GetStatus()==LoaderResultStatus::Success ? std::move(nca) : nullptr;
    }
    std::vector<ContentProviderEntry> ListEntriesFilter(std::optional<LoaderTitleType> title,
        std::optional<LoaderContentRecordType> type,std::optional<uint64_t> id) const override {
        std::vector<ContentProviderEntry> result;
        if (fallback) for (auto entry:fallback->ListEntriesFilter(title,type,id)) if (!owned.count(entry.titleID)) result.push_back(entry);
        for (const auto& [key,e]:entries) if ((!title || *title==e.title_type) && (!type || *type==key.second) && (!id || *id==key.first))
            result.push_back({key.first,key.second});
        std::sort(result.begin(),result.end(),[](const auto& x,const auto& y) { return std::tie(x.titleID,x.type)<std::tie(y.titleID,y.type); });
        result.erase(std::unique(result.begin(),result.end(),[](const auto& x,const auto& y) { return x.titleID==y.titleID && x.type==y.type; }),result.end());
        return result;
    }
};
}

struct AppLoader_NX::Impl {
    LoaderResultStatus status=LoaderResultStatus::ErrorBadPFSHeader;
    std::shared_ptr<nx::Session> session;
    uint32_t app=Absent,selected=Absent,base=Absent,control_snapshot=Absent;
    uint64_t program=0;
    std::vector<uint64_t> programs;
    std::map<std::array<uint8_t,32>,uint32_t> identities;
    std::vector<uint8_t> visited;
    NativeMeta base_meta,selected_meta;
    FileSys::VirtualFile base_file,update_file;
    std::shared_ptr<FileSys::NCA> base_nca,active_nca,control;
    std::unique_ptr<AppLoader_DeconstructedRomDirectory> executable;
    std::vector<NativeMeta> dlcs;
    std::shared_ptr<SelectedContent> provider=std::make_shared<SelectedContent>();
    // Extracted-tree snapshot (exefs/ + romfs/, no CNMT): a complete view.
    bool tree=false, standalone_ok=false;
    FileSys::VirtualDir tree_exefs;
    FileSys::VirtualFile tree_romfs;
    std::unique_ptr<FileSys::NACP> tree_nacp;

    // A snapshot without a native Content ID whose root holds exefs/.
    bool IsTree(uint32_t si) const {
        const auto& s=session->archive().snapshots()[si];
        static constexpr std::array<uint8_t,16> none{};
        if (s.content_id!=none || (s.flags&2) || s.type==2) return false;
        auto root=FileSys::NxSnapshotDirectory(session,si);
        return root && root->GetSubdirectory("exefs");
    }
    // Load an extracted-tree view. Its ExeFS is its own (never borrowed from a
    // base); NACP comes from the Catalog, the snapshot's own or its base's.
    void OpenTree(const nx::Application& owner,uint32_t pi) {
        const auto& a=session->archive(); const auto& s=a.snapshots()[selected];
        auto root=FileSys::NxSnapshotDirectory(session,selected);
        auto exefs=root ? root->GetSubdirectory("exefs") : nullptr;
        if (!exefs) return;
        program=a.programs()[pi].id;
        base=Base(selected); if (base==Absent) base=selected;
        auto main=exefs->GetFile("main"); std::array<uint8_t,96> nso{};
        if (!main || main->Read(nso.data(),nso.size(),0)!=nso.size() || std::memcmp(nso.data(),"NSO0",4)) return;
        static constexpr std::array<uint8_t,32> no_build{};
        if (s.build_id!=no_build && !std::equal(s.build_id.begin(),s.build_id.end(),nso.begin()+64)) return;
        auto npdm=exefs->GetFile("main.npdm");
        FileSys::ProgramMetadata metadata;
        if (!npdm || metadata.Load(npdm)!=LoaderResultStatus::Success || metadata.GetTitleID()!=program) {
            status=LoaderResultStatus::ErrorBadNPDMHeader; return;
        }
        for (uint32_t si:{selected,base}) {
            const auto index=a.snapshots()[si].nacp_index;
            if (index==Absent) continue;
            auto bytes=session->media(index);
            if (!bytes || bytes.value->size()<sizeof(FileSys::RawNACP)) return;
            tree_nacp=std::make_unique<FileSys::NACP>(std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>(bytes.value->begin(),bytes.value->end()),"control.nacp"));
            control_snapshot=si;
            break;
        }
        if (auto romfs=root->GetSubdirectory("romfs")) {
            tree_romfs=FileSys::CreateRomFS(romfs);
            if (!tree_romfs) return;
        }
        tree_exefs=exefs;
        executable=std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs,false);
        executable->SetApplyExeFSMods(true);
        // Own the title so installed NAND/SD updates never replace this view.
        provider->owned.insert(owner.id);
        for (uint32_t p=owner.first_program;p<owner.first_program+owner.program_count;++p) {
            provider->owned.insert(a.programs()[p].id);
            provider->owned.insert(FileSys::GetUpdateTitleID(a.programs()[p].id));
        }
        tree=true;
        status=LoaderResultStatus::Success;
    }

    bool ContentView() {
        const auto& a=session->archive(); const auto& owner=a.applications()[app];
        provider->owned.insert(owner.id);
        for (uint32_t p=owner.first_program;p<owner.first_program+owner.program_count;++p) provider->owned.insert(a.programs()[p].id);
        provider->owned.insert(base_meta.cnmt->GetTitleID()); provider->owned.insert(selected_meta.cnmt->GetTitleID());
        if (base_meta.related) provider->owned.insert(base_meta.related);
        using Slot=std::tuple<LoaderContentRecordType,uint8_t,uint64_t>;
        const auto slot_for=[](const NativeMeta& meta,const FileSys::ContentRecord& r) {
            uint64_t owner=0;
            if (r.type==LoaderContentRecordType::Control) Header(Content(meta,r),2,&owner);
            return Slot{r.type,r.id_offset,owner};
        };
        std::map<Slot,const FileSys::ContentRecord*> originals,updates;
        for (const auto& r:base_meta.cnmt->GetContentRecords()) {
            if (r.type==LoaderContentRecordType::DeltaFragment || r.type==LoaderContentRecordType::Meta) continue;
            if (!originals.emplace(slot_for(base_meta,r),&r).second) return false;
        }
        for (const auto& r:selected_meta.cnmt->GetContentRecords()) {
            if (r.type==LoaderContentRecordType::DeltaFragment || r.type==LoaderContentRecordType::Meta) continue;
            if (!updates.emplace(slot_for(selected_meta,r),&r).second) return false;
        }
        const auto add=[&](uint64_t id,LoaderContentRecordType type,SelectedContent::Entry entry) {
            provider->owned.insert(id);
            return provider->entries.emplace(SelectedContent::Key{id,type},std::move(entry)).second;
        };
        std::set<uint64_t> associations;
        const auto append=[&](Slot slot,const FileSys::ContentRecord* original,const FileSys::ContentRecord* update) {
            auto old=original ? Content(base_meta,*original) : nullptr;
            auto current=update ? Content(selected_meta,*update) : old;
            uint64_t id=0; std::array<uint8_t,32> h{};
            if (!current || current->Read(h.data(),h.size(),0x200)!=h.size() || std::memcmp(h.data(),"DNCA",4)) return false;
            id=U64(h.data()+16);
            SelectedContent::Entry entry{LoaderTitleType::Application,a.snapshots()[selected].numeric_version,current,update && selected!=base ? old : nullptr};
            const auto type=std::get<0>(slot);
            if (type==LoaderContentRecordType::Program) {
                if (!old || !associations.insert(id).second) return false;
                auto p=std::find_if(a.programs().begin(),a.programs().end(),[&](const nx::Program& item) { return item.application_index==app && item.id==id; });
                if (p==a.programs().end()) return false;
                const auto pi=static_cast<uint32_t>(p-a.programs().begin());
                uint32_t matching=Absent;
                for (uint32_t si=owner.first_snapshot;si<owner.first_snapshot+owner.snapshot_count;++si) {
                    const auto& s=a.snapshots()[si]; const auto& active=a.snapshots()[selected];
                    if (s.program_index==pi && s.type==active.type && s.numeric_version==active.numeric_version && s.content_id==active.content_id) {
                        if (matching!=Absent) return false;
                        matching=si;
                    }
                }
                if (matching==Absent || !Dependencies(matching)) return false;
                const auto dependency=Base(matching);
                if (dependency==Absent || a.snapshots()[dependency].content_id!=a.snapshots()[base].content_id ||
                    a.snapshots()[dependency].numeric_version!=a.snapshots()[base].numeric_version) return false;
                auto native_base=id!=program && entry.base ? std::make_shared<FileSys::NCA>(entry.base) : nullptr;
                if (native_base && native_base->GetStatus()!=LoaderResultStatus::Success) return false;
                auto native=id==program ? active_nca : std::make_shared<FileSys::NCA>(current,native_base.get());
                if (!native || native->GetStatus()!=LoaderResultStatus::Success || !native->GetExeFS()) return false;
                auto main=native->GetExeFS()->GetFile("main"); auto npdm=native->GetExeFS()->GetFile("main.npdm");
                std::array<uint8_t,96> executable_header{}; FileSys::ProgramMetadata metadata;
                if (!main || main->Read(executable_header.data(),executable_header.size(),0)!=executable_header.size() ||
                    std::memcmp(executable_header.data(),"NSO0",4) ||
                    !std::equal(a.snapshots()[matching].build_id.begin(),a.snapshots()[matching].build_id.end(),executable_header.begin()+64) ||
                    !npdm || metadata.Load(npdm)!=LoaderResultStatus::Success || metadata.GetTitleID()!=id) return false;
            }
            if (!add(id,type,entry)) return false;
            if (old && update && selected!=base) {
                if (old->Read(h.data(),h.size(),0x200)!=h.size()) return false;
                const auto base_id=U64(h.data()+16);
                if (base_id!=id && (type==LoaderContentRecordType::Program || type==LoaderContentRecordType::Control || !add(base_id,type,entry))) return false;
            }
            return true;
        };
        for (const auto& [slot,original]:originals) {
            auto it=updates.find(slot);
            // Runtime native inheritance is separate from display metadata:
            // unchanged Base Control stays available under its exact owner.
            if (!append(slot,original,it==updates.end() ? nullptr : it->second)) return false;
            if (it!=updates.end()) updates.erase(it);
        }
        for (const auto& [slot,update]:updates) if (!append(slot,nullptr,update)) return false;
        if (associations.size()!=owner.program_count) return false;
        for (const auto& meta:dlcs) for (const auto& r:meta.cnmt->GetContentRecords()) {
            if (r.type==LoaderContentRecordType::DeltaFragment || r.type==LoaderContentRecordType::Meta) continue;
            if (!add(meta.cnmt->GetTitleID(),r.type,{LoaderTitleType::AOC,meta.cnmt->GetTitleVersion(),Content(meta,r),nullptr})) return false;
        }
        return true;
    }

    // Updates the user turned off in the game properties are skipped, as for
    // separate update files: the next most recent one, or the base, is used.
    // An update added from an update folder is standalone (type 3): it has no base dependency.
    static bool IsUpdate(uint8_t type) { return type==1 || type==3; }
    static bool UpdateDisabled(uint64_t program_id,uint32_t version) {
        const auto it=loaderSettings.disabled_addons.find(program_id);
        if (it==loaderSettings.disabled_addons.end()) return false;
        const auto key="Update@"+std::to_string(version);
        return std::find(it->second.begin(),it->second.end(),"Update")!=it->second.end() ||
               std::find(it->second.begin(),it->second.end(),key)!=it->second.end();
    }
    static uint32_t Latest(const nx::Archive& archive,uint32_t pi) {
        uint32_t found=Absent;
        const auto program_id=archive.programs()[pi].id;
        for (uint32_t i=0;i<archive.snapshots().size();++i) {
            const auto& s=archive.snapshots()[i];
            if (s.program_index!=pi || s.type==2) continue;
            if (IsUpdate(s.type) && UpdateDisabled(program_id,s.numeric_version)) continue;
            if (found==Absent || s.numeric_version>archive.snapshots()[found].numeric_version ||
                (s.numeric_version==archive.snapshots()[found].numeric_version && s.type>archive.snapshots()[found].type)) found=i;
            else if (s.numeric_version==archive.snapshots()[found].numeric_version && s.type==archive.snapshots()[found].type) return Absent;
        }
        return found;
    }
    bool Dependencies(uint32_t si,unsigned depth=0) {
        if (depth>64 || visited[si]==1) return false;
        if (visited[si]==2) return true;
        visited[si]=1; bool valid=true; unsigned bases=0;
        const auto& a=session->archive(); const auto& owner=a.applications()[app];
        const auto& source=a.snapshots()[si];
        a.dependencies(si,[&](const nx::Dependency& d) {
            if (d.kind==3) return true;
            auto it=identities.find(d.target_identity);
            if (it==identities.end() || it->second==si || it->second<owner.first_snapshot || it->second>=owner.first_snapshot+owner.snapshot_count) return valid=false;
            const auto& target=a.snapshots()[it->second];
            if ((d.content_id && *d.content_id!=target.content_id) || target.type==2) return valid=false;
            if (d.kind==1 && (++bases!=1 || source.type!=1 || target.type!=0 || target.program_index!=source.program_index)) return valid=false;
            return valid=Dependencies(it->second,depth+1);
        });
        if (source.type==1 && bases!=1 && !(bases==0 && standalone_ok && depth==0)) valid=false;
        visited[si]=valid ? 2 : 0; return valid;
    }
    uint32_t Base(uint32_t si) const {
        if (session->archive().snapshots()[si].type==0) return si;
        uint32_t result=Absent;
        session->archive().dependencies(si,[&](const nx::Dependency& d) {
            if (d.kind==1) { auto it=identities.find(d.target_identity); if (it!=identities.end()) result=it->second; }
            return true;
        });
        return result;
    }
    void Open(FileSys::VirtualFile file,uint64_t requested,size_t offset,std::string_view path,
              const std::optional<std::array<uint8_t,32>>& exact,nx::SessionOptions options,
              std::optional<uint64_t> application_id) {
        auto opened=FileSys::OpenNxSession(std::move(file),path,options);
        if (!opened) return;
        session=std::move(*opened.value); const auto& a=session->archive();
        for (uint32_t i=0;i<a.snapshots().size();++i) identities.emplace(a.snapshots()[i].identity,i);
        visited.resize(a.snapshots().size());
        for (uint32_t i=0;i<a.applications().size();++i) {
            const auto& owner=a.applications()[i];
            if (owner.platform!=1) continue;
            if (application_id && *application_id!=owner.id) continue;
            bool matches=requested==0;
            for (auto p=owner.first_program;p!=Absent && p<owner.first_program+owner.program_count;++p) matches|=a.programs()[p].id==requested;
            if (matches) { if (app!=Absent) return; app=i; }
        }
        if (app==Absent) return;
        const auto& owner=a.applications()[app]; uint32_t pi=Absent;
        for (auto p=owner.first_program;p!=Absent && p<owner.first_program+owner.program_count;++p) {
            programs.push_back(a.programs()[p].id);
            if ((requested && a.programs()[p].id==requested) ||
                (!requested && a.programs()[p].kind==0)) { if (pi!=Absent) return; pi=p; }
        }
        if (pi==Absent) return;
        selected=Latest(a,pi);
        if (exact) { auto it=identities.find(*exact); if (it==identities.end()) return; selected=it->second; }
        const bool tree_view=selected!=Absent && IsTree(selected);
        standalone_ok=tree_view;
        if (selected==Absent || a.snapshots()[selected].program_index==Absent ||
            a.programs()[a.snapshots()[selected].program_index].application_index!=app || !Dependencies(selected)) return;
        pi=a.snapshots()[selected].program_index;
        if (exact && requested && a.programs()[pi].id!=requested) return;
        if (tree_view) {
            // Trees name each Program directly; a native ID offset does not apply.
            if (!offset) OpenTree(owner,pi);
            return;
        }
        base=Base(selected);
        if (base==Absent || !ReadMeta(session,base,owner.id,base_meta)) return;
        if (offset) {
            // program_index is the native CNMT ID offset, never ID arithmetic.
            if (offset>255 || exact) return;
            uint64_t target=0;
            for (const auto& record:base_meta.cnmt->GetContentRecords()) if (record.type==LoaderContentRecordType::Program && record.id_offset==offset) {
                if (target || !Header(Content(base_meta,record),0,&target)) return;
            }
            auto it=std::find_if(a.programs().begin(),a.programs().end(),[&](const nx::Program& p){return p.application_index==app && p.id==target;});
            // The frontend supplies the base program ID with a native index.
            // requested selected the owning application/snapshot above; the CNMT
            // mapping selects the executable, whose ID can differ from requested.
            if (!target || it==a.programs().end()) return;
            const auto original=selected; selected=Absent; pi=static_cast<uint32_t>(it-a.programs().begin());
            for (uint32_t si=owner.first_snapshot;si<owner.first_snapshot+owner.snapshot_count;++si) {
                const auto& s=a.snapshots()[si]; const auto& primary=a.snapshots()[original];
                if (s.program_index==pi && s.type==primary.type && s.numeric_version==primary.numeric_version && s.content_id==primary.content_id) { if (selected!=Absent) return; selected=si; }
            }
            if (selected==Absent || !Dependencies(selected)) return;
            base=Base(selected); if (!ReadMeta(session,base,owner.id,base_meta)) return;
        }
        program=a.programs()[pi].id; uint8_t native_offset=0;
        base_file=Find(base_meta,LoaderContentRecordType::Program,program,&native_offset);
        if (!base_file) return;
        base_nca=std::make_shared<FileSys::NCA>(base_file);
        if (base_nca->GetStatus()!=LoaderResultStatus::Success) return;
        if (!ReadMeta(session,selected,owner.id,selected_meta)) return;
        active_nca=base_nca;
        for (const auto& record:selected_meta.cnmt->GetContentRecords()) if (record.type==LoaderContentRecordType::Program && record.id_offset==native_offset) {
            auto candidate=Content(selected_meta,record); uint64_t id=0;
            if (!Header(candidate,0,&id) || id!=program) return;
            if (selected!=base) { update_file=candidate; active_nca=std::make_shared<FileSys::NCA>(candidate,base_nca.get()); }
        }
        if (active_nca->GetStatus()!=LoaderResultStatus::Success || !active_nca->GetExeFS()) return;
        auto main=active_nca->GetExeFS()->GetFile("main"); std::array<uint8_t,96> nso{};
        if (!main || main->Read(nso.data(),nso.size(),0)!=nso.size() || std::memcmp(nso.data(),"NSO0",4) ||
            !std::equal(a.snapshots()[selected].build_id.begin(),a.snapshots()[selected].build_id.end(),nso.begin()+64)) return;
        auto npdm=active_nca->GetExeFS()->GetFile("main.npdm");
        FileSys::ProgramMetadata metadata;
        if (!npdm || metadata.Load(npdm)!=LoaderResultStatus::Success || metadata.GetTitleID()!=program) {
            status=LoaderResultStatus::ErrorBadNPDMHeader; return;
        }
        executable=std::make_unique<AppLoader_DeconstructedRomDirectory>(active_nca->GetExeFS(),false);
        executable->SetApplyExeFSMods(true);
        auto control_file=Find(selected_meta,LoaderContentRecordType::Control,program,nullptr,native_offset);
        if (!control_file && a.snapshots()[selected].nacp_index!=Absent) return;
        const bool own_control=control_file!=nullptr;
        auto base_control_file=Find(base_meta,LoaderContentRecordType::Control,program,nullptr,native_offset);
        if (!control_file) control_file=base_control_file;
        if (control_file) {
            auto base_control=base_control_file ? std::make_shared<FileSys::NCA>(base_control_file) : nullptr;
            if (base_control && base_control->GetStatus()!=LoaderResultStatus::Success) return;
            control=(!own_control || selected==base) ? base_control : std::make_shared<FileSys::NCA>(control_file,base_control.get());
            control_snapshot=own_control ? selected : base;
            if (!control || control->GetStatus()!=LoaderResultStatus::Success) return;
        }
        std::map<uint64_t,uint32_t> latest_dlc;
        for (uint32_t si=owner.first_snapshot;si<owner.first_snapshot+owner.snapshot_count;++si) if (a.snapshots()[si].type==2) {
            if (a.snapshots()[si].addon_title_id()) continue;  // no native CNMT to expose
            NativeMeta meta;
            if (!ReadMeta(session,si,owner.id,meta) || !Dependencies(si)) return;
            provider->owned.insert(meta.cnmt->GetTitleID());
            if (meta.minimum>a.snapshots()[selected].numeric_version) continue;
            const auto title=meta.cnmt->GetTitleID();
            auto it=latest_dlc.find(title);
            if (it==latest_dlc.end() || a.snapshots()[si].numeric_version>a.snapshots()[it->second].numeric_version) latest_dlc[title]=si;
            else if (a.snapshots()[si].numeric_version==a.snapshots()[it->second].numeric_version) return;
        }
        for (const auto& [id,si]:latest_dlc) { (void)id; NativeMeta meta; if (!ReadMeta(session,si,owner.id,meta)) return; dlcs.push_back(std::move(meta)); }
        if (!ContentView()) return;
        status=LoaderResultStatus::Success;
    }
};

AppLoader_NX::AppLoader_NX(FileSys::VirtualFile source,uint64_t program,size_t index,std::string_view path,std::optional<std::array<uint8_t,32>> snapshot,nx::SessionOptions options,std::optional<uint64_t> application_id)
    :AppLoader(source),impl_(std::make_unique<Impl>()) { impl_->Open(std::move(source),program,index,path,snapshot,options,application_id); }
AppLoader_NX::~AppLoader_NX()=default;
LoaderFileType AppLoader_NX::IdentifyType(const FileSys::VirtualFile& source) {
    constexpr size_t PageSize = 4096;
    if (!source) return LoaderFileType::Error;
    // The header is at the start, or one page in when it is being recovered.
    for (const size_t offset : {size_t{0}, PageSize}) {
        std::array<uint8_t,4> magic{};
        if (source->GetSize() >= offset + magic.size() &&
            source->Read(magic.data(), magic.size(), offset) == magic.size() &&
            std::memcmp(magic.data(), "NXAR", magic.size()) == 0) return LoaderFileType::NX;
    }
    return LoaderFileType::Error;
}
std::optional<AppLoader_NX::CatalogMetadata> AppLoader_NX::ReadCatalogMetadata(const FileSys::VirtualFile& file) {
    auto catalog=FileSys::OpenNxCatalog(file);
    if (!catalog) return std::nullopt;
    auto& archive=catalog->archive;
    CatalogMetadata result;
    uint32_t app=Absent, program=Absent;
    for (uint32_t i=0;i<archive.applications().size();++i) {
        const auto& owner=archive.applications()[i];
        if (owner.platform!=1) continue;
        if (app!=Absent) return std::nullopt;
        app=i;
        for (uint32_t p=owner.first_program;p!=Absent && p<owner.first_program+owner.program_count;++p) {
            const auto& entry=archive.programs()[p];
            result.programs.push_back(entry.id);
            if (entry.kind!=0) continue;
            if (program!=Absent) return std::nullopt;
            program=p;
        }
    }
    if (program==Absent) return std::nullopt;
    const auto selected=Impl::Latest(archive,program);
    if (selected==Absent) return std::nullopt;
    result.program=archive.programs()[program].id;
    const auto title=PreferredLocalized(archive,app,[&](uint32_t i) { return !archive.localized_title(i).empty(); });
    result.title=title!=Absent ? archive.localized_title(title) : archive.title(app);
    result.publisher=archive.publisher(app);
    result.version=archive.snapshot_display_version(selected);
    const auto& snapshot=archive.snapshots()[selected];
    if (result.version.empty() && Impl::IsUpdate(snapshot.type)) result.version="v"+std::to_string(snapshot.numeric_version);
    const auto local=PreferredLocalized(archive,app,[&](uint32_t i) { return archive.localized_entries()[i].icon_index!=Absent; });
    const auto icon=local!=Absent ? archive.localized_entries()[local].icon_index : archive.applications()[app].icon_index;
    if (icon!=Absent) {
        auto bytes=archive.read_media(icon);
        if (bytes) result.icon=std::move(*bytes.value);
    }
    return result;
}
LoaderResultStatus AppLoader_NX::GetStatus() const { return impl_->status; }
uint32_t AppLoader_NX::SelectedSnapshot() const { return impl_->selected; }
uint32_t AppLoader_NX::SelectedVersion() const { return impl_->status==LoaderResultStatus::Success ? impl_->session->archive().snapshots()[impl_->selected].numeric_version : 0; }
bool AppLoader_NX::UpdateSelected() const { return impl_->status==LoaderResultStatus::Success && impl_->session->archive().snapshots()[impl_->selected].type==1; }
uint32_t AppLoader_NX::ControlSnapshot() const { return impl_->control_snapshot; }
std::shared_ptr<nx::Session> AppLoader_NX::Session() const { return impl_->session; }
FileSys::VirtualDir AppLoader_NX::ExeFS() const { return impl_->tree ? impl_->tree_exefs : impl_->active_nca ? impl_->active_nca->GetExeFS() : nullptr; }
AppLoader::LoadResult AppLoader_NX::Load(Systemloader& loader,ISystemModules& modules) {
    if (is_loaded) return {LoaderResultStatus::ErrorAlreadyLoaded,{}};
    if (impl_->status!=LoaderResultStatus::Success) return {impl_->status,{}};
    auto result=impl_->executable->Load(loader,modules);
    if (result.first!=LoaderResultStatus::Success) return result;
    auto romfs=std::make_shared<FileSys::RomFSFactory>(impl_->tree ? impl_->tree_romfs : impl_->active_nca->RomFS(),false,loader.GetContentProvider(),loader.GetFileSystemController(),impl_->provider);
    romfs->SetApplyMods(true);
    if (!loader.GetFileSystemController().RegisterProcess(result.second->process_id,impl_->program,std::move(romfs)))
        return {LoaderResultStatus::ErrorNotInitialized,{}};
    is_loaded=true; return result;
}
LoaderResultStatus AppLoader_NX::VerifyIntegrity(std::function<bool(size_t,size_t)> progress) {
    if (!impl_->session) return impl_->status;
    if (progress && !progress(0,1)) return LoaderResultStatus::ErrorCancelled;
    const auto result=impl_->session->verify_all();
    if (!result) return result.error.code==nx::ErrorCode::cancelled ? LoaderResultStatus::ErrorCancelled : LoaderResultStatus::ErrorBadPFSHeader;
    if (progress) progress(1,1);
    return LoaderResultStatus::Success;
}
LoaderResultStatus AppLoader_NX::ReadRomFS(FileSys::VirtualFile& out) { if (impl_->status!=LoaderResultStatus::Success) return impl_->status; out=impl_->tree ? impl_->tree_romfs : impl_->active_nca->RomFS(); return out ? LoaderResultStatus::Success : LoaderResultStatus::ErrorNoRomFS; }
LoaderResultStatus AppLoader_NX::ReadProgramId(uint64_t& id) { if (impl_->status!=LoaderResultStatus::Success) return impl_->status; id=impl_->program; return LoaderResultStatus::Success; }
LoaderResultStatus AppLoader_NX::ReadProgramIds(uint64_t* ids,uint32_t* count) {
    if (impl_->status!=LoaderResultStatus::Success) return impl_->status;
    if (!count) return LoaderResultStatus::ErrorNotImplemented;
    if (ids && *count<impl_->programs.size()) return LoaderResultStatus::ErrorBufferTooSmall;
    *count=static_cast<uint32_t>(impl_->programs.size()); if (ids) std::copy(impl_->programs.begin(),impl_->programs.end(),ids);
    return LoaderResultStatus::Success;
}
LoaderResultStatus AppLoader_NX::ReadControlData(FileSys::NACP& nacp) {
    if (impl_->status!=LoaderResultStatus::Success) return impl_->status;
    if (impl_->tree) {
        if (!impl_->tree_nacp) return LoaderResultStatus::ErrorNoControl;
        nacp=*impl_->tree_nacp; return LoaderResultStatus::Success;
    }
    if (!impl_->control) return LoaderResultStatus::ErrorNoControl;
    auto metadata=FileSys::ParseControlRomFS(impl_->control->RomFS());
    if (!metadata.first) return LoaderResultStatus::ErrorNoControl;
    nacp=*metadata.first; return LoaderResultStatus::Success;
}
LoaderResultStatus AppLoader_NX::ReadTitle(char* out,uint32_t* size) {
    if (impl_->status!=LoaderResultStatus::Success) return impl_->status;
    // Display source is an independent Catalog choice. It must never select
    // executable content or override version-specific process control metadata.
    const auto& archive=impl_->session->archive();
    const auto local=PreferredLocalized(archive,impl_->app,[&](uint32_t i) { return !archive.localized_title(i).empty(); });
    const auto title=local!=Absent ? archive.localized_title(local) : archive.title(impl_->app);
    if (title.empty()) return LoaderResultStatus::ErrorNoControl;
    return Copy(reinterpret_cast<const uint8_t*>(title.data()),title.size(),reinterpret_cast<uint8_t*>(out),size);
}
LoaderResultStatus AppLoader_NX::ReadIcon(uint8_t* out,uint32_t* size) {
    if (!impl_->session || impl_->app == Absent) return LoaderResultStatus::ErrorNoIcon;
    const auto& archive=impl_->session->archive();
    const auto local=PreferredLocalized(archive,impl_->app,[&](uint32_t i) { return archive.localized_entries()[i].icon_index!=Absent; });
    const auto icon=local!=Absent ? archive.localized_entries()[local].icon_index : archive.applications()[impl_->app].icon_index;
    if (icon==Absent) return LoaderResultStatus::ErrorNoIcon;
    auto bytes=impl_->session->media(icon);
    if (!bytes) return LoaderResultStatus::ErrorNoIcon;
    return Copy(bytes.value->data(),bytes.value->size(),out,size);
}
LoaderResultStatus AppLoader_NX::ReadOriginalIcon(uint8_t * out, uint32_t * size) const
{
    if (impl_->status != LoaderResultStatus::Success || !impl_->control)
        return LoaderResultStatus::ErrorNoIcon;

    auto icon = FileSys::ParseControlRomFS(impl_->control->RomFS()).second;
    if (!icon && impl_->control_snapshot != impl_->base)
    {
        const auto base = Find(impl_->base_meta, LoaderContentRecordType::Control, impl_->program);
        if (base)
        {
            const FileSys::NCA control(base);
            if (control.GetStatus() == LoaderResultStatus::Success)
                icon = FileSys::ParseControlRomFS(control.RomFS()).second;
        }
    }
    if (!icon || icon->GetSize() == 0 || icon->GetSize() > 1024 * 1024)
        return LoaderResultStatus::ErrorNoIcon;
    if (size == nullptr) return LoaderResultStatus::ErrorNotImplemented;
    const auto length = static_cast<uint32_t>(icon->GetSize());
    if (out && *size < length) return LoaderResultStatus::ErrorBufferTooSmall;
    *size = length;
    if (out && icon->Read(out, length, 0) != length) return LoaderResultStatus::ErrorNoIcon;
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_NX::ReadDisplayVersion(char* out,uint32_t* size) const {
    if (impl_->status!=LoaderResultStatus::Success) return impl_->status;
    const auto& s=impl_->session->archive().snapshots()[impl_->selected];
    std::string version{impl_->session->archive().snapshot_display_version(impl_->selected)};
    // An update added from a bare folder records no display version: show its number.
    if (version.empty() && (s.type==1 || s.type==3)) version="v"+std::to_string(s.numeric_version);
    if (version.empty()) return LoaderResultStatus::ErrorNoControl;
    return Copy(reinterpret_cast<const uint8_t*>(version.data()),version.size(),reinterpret_cast<uint8_t*>(out),size);
}
std::vector<AppLoader_NX::Update> AppLoader_NX::Updates() const {
    std::vector<Update> result;
    if (impl_->status!=LoaderResultStatus::Success || !impl_->session) return result;
    const auto& a=impl_->session->archive();
    for (uint32_t si=0;si<a.snapshots().size();++si) {
        const auto& s=a.snapshots()[si];
        if ((s.type!=1 && s.type!=3) || s.program_index>=a.programs().size() || a.programs()[s.program_index].id!=impl_->program) continue;
        result.push_back({s.numeric_version,std::string(a.snapshot_display_version(si)),si==impl_->selected});
    }
    std::sort(result.begin(),result.end(),[](const Update& x,const Update& y) { return x.version>y.version; });
    return result;
}
LoaderResultStatus AppLoader_NX::ReadNSOModules(Modules& modules) { return is_loaded ? impl_->executable->ReadNSOModules(modules) : LoaderResultStatus::ErrorNotInitialized; }
void AppLoader_NX::AddToManualContentProvider(IManualContentProvider& provider) const {
    if (impl_->status!=LoaderResultStatus::Success || impl_->tree) return;
    // Executable/RomFS are already resolved exactly; never register an update
    // under a guessed title ID or let sibling containers override this view.
    provider.AddEntry(LoaderTitleType::Application,LoaderContentRecordType::Program,impl_->program,impl_->base_meta.cnmt->GetTitleVersion(),new VirtualFileImpl(impl_->base_file));
    if (impl_->control) { auto content=impl_->control->BaseFile(); provider.AddEntry(LoaderTitleType::Application,LoaderContentRecordType::Control,impl_->program,impl_->selected_meta.cnmt->GetTitleVersion(),new VirtualFileImpl(content)); }
    for (const auto& meta:impl_->dlcs) for (const auto& record:meta.cnmt->GetContentRecords()) {
        auto content=Content(meta,record); if (content) provider.AddEntry(LoaderTitleType::AOC,record.type,meta.cnmt->GetTitleID(),meta.cnmt->GetTitleVersion(),new VirtualFileImpl(content));
    }
}
bool AppLoader_NX::ActiveContent(ActiveProgramContent& out) const {
    // A tree view has no native Program NCA to hand out.
    if (impl_->status!=LoaderResultStatus::Success || impl_->tree) return false;
    auto base=std::make_unique<FileSys::NCA>(impl_->base_file);
    std::unique_ptr<FileSys::NCA> update;
    if (impl_->update_file) update=std::make_unique<FileSys::NCA>(impl_->update_file,base.get());
    if (base->GetStatus()!=LoaderResultStatus::Success || (update && update->GetStatus()!=LoaderResultStatus::Success)) return false;
    out={}; out.update_available=out.update_enabled=impl_->selected!=impl_->base;
    out.base_program=base.release(); out.update_program=update.release(); return true;
}
FileSys::ContentProvider& AppLoader_NX::ContentProvider(FileSys::ContentProvider& fallback) const {
    std::call_once(impl_->provider->fallback_ready,[&] { impl_->provider->fallback=&fallback; });
    return *impl_->provider;
}
} // namespace Loader
