// Exercise the production loader module with synthetic decrypted containers.
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/filesystem.h"
#include "core/file_sys/fssystem/fssystem_bucket_tree.h"
#include "core/file_sys/fssystem/fssystem_nca_header.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/partition_filesystem.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/loader/nca.h"
#include "core/loader/nsp.h"
#include "core/loader/xci.h"
#include "loader_settings.h"
#include "system_loader.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
using namespace FileSys;
constexpr u64 Title = 0x0100123400000000ULL;
void Check(bool value, const char *message)
{
    if (!value)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}
template <class T> void Put(std::vector<u8> &out, size_t off, const T &value)
{
    if (out.size() < off + sizeof(T))
        out.resize(off + sizeof(T));
    std::memcpy(out.data() + off, &value, sizeof(T));
}
using Files = std::vector<std::pair<std::string, std::vector<u8>>>;
std::vector<u8> Pfs(const Files &files, bool hfs = false)
{
    size_t names = 0, data = 0;
    for (auto &f : files)
    {
        names += f.first.size() + 1;
        data += f.second.size();
    }
    size_t stride = hfs ? 0x40 : 0x18, head = 0x10 + stride * files.size() + names;
    std::vector<u8> out(head + data);
    std::memcpy(out.data(), hfs ? "HFS0" : "PFS0", 4);
    Put(out, 4, u32(files.size()));
    Put(out, 8, u32(names));
    size_t name = 0, off = 0;
    for (size_t i = 0; i < files.size(); ++i)
    {
        auto &f = files[i];
        Put(out, 0x10 + i * stride, u64(off));
        Put(out, 0x18 + i * stride, u64(f.second.size()));
        Put(out, 0x20 + i * stride, u32(name));
        std::memcpy(out.data() + 0x10 + stride * files.size() + name, f.first.c_str(),
                    f.first.size() + 1);
        std::memcpy(out.data() + head + off, f.second.data(), f.second.size());
        name += f.first.size() + 1;
        off += f.second.size();
    }
    return out;
}
std::vector<u8> Nca(NcaHeader::ContentType type, std::vector<u8> pfs = {}, bool bad_romfs = false)
{
    NcaHeader header{};
    header.magic = NcaHeader::MagicDecrypted;
    header.content_type = type;
    header.program_id = Title;
    header.sdk_addon_version = 0x000b0000;
    std::vector<u8> out(0xc00);
    if (!pfs.empty() || bad_romfs)
    {
        auto size = (pfs.size() + 511) / 512 * 512;
        if (!size)
            size = 512;
        if (bad_romfs)
            size = 0x10000;
        header.fs_info[0].start_sector = 6;
        header.fs_info[0].end_sector = 6 + u32((512 + size) / 512);
        NcaFsHeader fs{};
        fs.fs_type = bad_romfs ? NcaFsHeader::FsType::RomFs : NcaFsHeader::FsType::PartitionFs;
        fs.encryption_type = NcaFsHeader::EncryptionType::None;
        fs.hash_type = NcaFsHeader::HashType::HierarchicalSha256Hash;
        auto &hash = fs.hash_data.hierarchical_sha256_data;
        hash.hash_block_size = bad_romfs ? 4096 : 512;
        hash.hash_layer_count = 2;
        hash.hash_layer_region[0].size = 32;
        hash.hash_layer_region[1].offset = 512;
        hash.hash_layer_region[1].size = size;
        if (bad_romfs)
        {
            fs.compression_info.bucket.offset = 512;
            fs.compression_info.bucket.size = 0x8000;
            BucketTree::Header table{};
            table.Format(1);
            std::memcpy(fs.compression_info.bucket.header.data(), &table, sizeof(table));
        }
        Put(out, 0x400, fs);
        out.resize(0xc00 + 512 + size);
        std::memcpy(out.data() + 0xe00, pfs.data(), pfs.size());
    }
    header.content_size = out.size();
    Put(out, 0, header);
    return out;
}
VirtualFile File(std::vector<u8> data, std::string name = "fixture.nca")
{
    return std::make_shared<VectorVfsFile>(std::move(data), std::move(name));
}
std::vector<u8> Nsp(bool damaged = false, bool malformed_meta = false)
{
    const std::string id(32, '0');
    CNMTHeader h{};
    h.title_id = Title;
    h.type = LoaderTitleType::Application;
    h.number_content_entries = 1;
    h.table_offset = sizeof(OptionalHeader);
    ContentRecord rec{};
    rec.type = LoaderContentRecordType::Program;
    std::vector<u8> cnmt;
    Put(cnmt, 0, h);
    Put(cnmt, sizeof(h), OptionalHeader{});
    Put(cnmt, sizeof(h) + sizeof(OptionalHeader), rec);
    auto meta = Nca(NcaHeader::ContentType::Meta, Pfs({{"fixture.cnmt", cnmt}}));
    if (malformed_meta)
        meta.resize(10);
    return Pfs({{id + ".cnmt.nca", meta},
                {id + ".nca", Nca(NcaHeader::ContentType::Program, {}, damaged)}});
}
std::vector<u8> Xci(std::vector<u8> secure)
{
    GamecardHeader h{};
    std::memcpy(&h.magic, "DXCI", 4);
    h.hfs_offset = sizeof(h);
    std::vector<u8> out(sizeof(h));
    Put(out, 0, h);
    auto root = Pfs({{"normal", Pfs({}, true)}, {"secure", std::move(secure)}}, true);
    out.insert(out.end(), root.begin(), root.end());
    return out;
}
struct Modules : ISystemModules
{
    void StartEmulation() override
    {
        std::abort();
    }
    void StopEmulation(bool) override
    {
    }
    bool IsValid() const override
    {
        return false;
    }
    ISystemloader &Systemloader() override
    {
        std::abort();
    }
    IOperatingSystem &OperatingSystem() override
    {
        std::abort();
    }
    IVideo &Video() override
    {
        std::abort();
    }
    ICpu &Cpu() override
    {
        std::abort();
    }
};
int main(int argc, char **argv)
{
    Modules modules;
    ::Systemloader loader(modules);
    auto &fsc = loader.GetFileSystemController();
    auto bad = File(std::vector<u8>(0x400));
    Check(!NCA(bad).HasRomFSSectionReadFailure(), "bad magic is not recoverable");
    Check(Loader::AppLoader_NCA::IdentifyType(bad) == LoaderFileType::Error,
          "invalid header rejected");
    auto truncated = Nca(NcaHeader::ContentType::Program);
    truncated.resize(0x230);
    Check(!NCA(File(truncated)).HasRomFSSectionReadFailure(), "truncated main header rejected");
    auto short_fs = Nca(NcaHeader::ContentType::Program, {}, true);
    short_fs.resize(0x410);
    Check(!NCA(File(short_fs)).HasRomFSSectionReadFailure(), "truncated section header rejected");
    auto damaged = File(Nsp(true), "sparse.dnsp");
    Check(Loader::AppLoader_NSP::IdentifyType(damaged) == LoaderFileType::NSP,
          "incomplete base detected");
    Check(NSP(File(Nsp(false, true))).GetNCA(Title, LoaderContentRecordType::Program) == nullptr,
          "invalid metadata skipped safely");
    auto sparse_xci = File(Xci(Nsp(true)), "sparse.dxci");
    Check(Loader::AppLoader_XCI::IdentifyType(sparse_xci) == LoaderFileType::XCI,
          "XCI incomplete base detected");
    ContentProviderUnion providers;
    ManualContentProvider manual;
    providers.SetSlot(ContentProviderUnionSlot::FrontendManual, &manual);
    auto empty = std::make_shared<VectorVfsDirectory>(
        std::vector<VirtualFile>{File({1}, "main"), File({1}, "main.npdm")});
    const auto good =
        File(Nca(NcaHeader::ContentType::Program, Pfs({{"main", {1}}, {"main.npdm", {1}}})));
    Check(NCA(good).GetStatus() == LoaderResultStatus::Success, "synthetic executable NCA valid");
    PatchManager pm(Title, fsc, providers);
    Check(pm.PatchExeFS(empty) != nullptr, "ordinary base without updates still works");
    auto disable = [&](std::vector<std::string> flags) {
        loaderSettings.disabled_addons[Title] = std::move(flags);
    };
    manual.AddEntry(LoaderTitleType::Update, LoaderContentRecordType::Program, Title | 0x800, 1,
                    good);
    manual.AddEntry(LoaderTitleType::Update, LoaderContentRecordType::Program, Title | 0x800, 2,
                    bad);
    Check(pm.PatchExeFS(empty) == nullptr, "broken selected version must not fall back");
    disable({"Update@2"});
    Check(pm.PatchExeFS(nullptr) != nullptr, "disabled newest selects older enabled version");
    disable({"Update@1", "Update@2"});
    Check(pm.PatchExeFS(nullptr, true, good, 3) == nullptr,
          "all versions disabled blocks packed fallback");
    disable({"Update"});
    Check(pm.PatchExeFS(nullptr) == nullptr, "global disable blocks update executable");
    manual.ClearAllEntries();
    disable({});
    manual.AddEntry(LoaderTitleType::Update, LoaderContentRecordType::Program, Title | 0x800, 1,
                    good);
    manual.AddEntry(LoaderTitleType::Update, LoaderContentRecordType::Control, Title | 0x800, 2,
                    good);
    Check(pm.PatchExeFS(empty, true, good, 3) == nullptr,
          "selected version without Program cannot use older or packed executable");
    manual.ClearAllEntries();
    disable({"Update@3"});
    Check(pm.PatchExeFS(nullptr, true, good, 3) == nullptr, "packed version disable respected");
    disable({});
    Check(pm.PatchExeFS(nullptr, true, good, 3) != nullptr, "packed fallback enabled");
    Check(pm.PatchExeFS(nullptr, false, good, 3) == nullptr, "apply_update false respected");
    manual.AddEntry(LoaderTitleType::Update, LoaderContentRecordType::Program, Title | 0x800, 4,
                    File(Nca(NcaHeader::ContentType::Program)));
    Check(pm.PatchExeFS(empty) != nullptr, "valid update without ExeFS keeps base executable");
    Check(pm.PatchExeFS(nullptr) == nullptr, "no ExeFS in base or update must fail");
    manual.ClearAllEntries();
    // A genuine RomFS-only program NCA has no executable partition.
    auto romfs_only_bytes = Nca(NcaHeader::ContentType::Program, Pfs({{"data", {1}}}));
    NcaFsHeader romfs_header{};
    std::memcpy(&romfs_header, romfs_only_bytes.data() + 0x400, sizeof(romfs_header));
    romfs_header.fs_type = NcaFsHeader::FsType::RomFs;
    Put(romfs_only_bytes, 0x400, romfs_header);
    const auto romfs_only = File(romfs_only_bytes);
    Check(NCA(romfs_only).RomFS() && !NCA(romfs_only).HasExeFSSection(),
          "RomFS-only fixture has data and no executable partition");
    manual.AddEntry(LoaderTitleType::Update, LoaderContentRecordType::Program, Title | 0x800, 5,
                    romfs_only);
    const auto retained = pm.PatchExeFS(empty);
    Check(retained && retained->GetFile("main") == empty->GetFile("main"),
          "RomFS-only update preserves the actual base executable");
    Check(pm.PatchExeFS(nullptr) == nullptr, "RomFS-only update cannot boot without base ExeFS");
    const auto invalid_base = std::make_shared<VectorVfsDirectory>();
    Check(pm.PatchExeFS(invalid_base) == nullptr, "RomFS-only update requires a usable base ExeFS");
    manual.ClearAllEntries();
    Check(pm.PatchExeFS(empty, true, romfs_only, 5) != nullptr,
          "packed RomFS-only update retains base ExeFS");
    for (const auto &invalid_partition :
         {Pfs({{"main", {1}}}),
          Pfs({{"main", {1}}, {"NintendoLogo.png", {1}}, {"StartupMovie.gif", {1}}}),
          std::vector<u8>{1, 2, 3}})
    {
        const auto broken_exefs = File(Nca(NcaHeader::ContentType::Program, invalid_partition));
        const NCA broken(broken_exefs);
        Check(broken.GetStatus() == LoaderResultStatus::Success && broken.HasExeFSSection() &&
                  !broken.GetExeFS(),
              "malformed executable partition is distinguished from absence");
        manual.AddEntry(LoaderTitleType::Update, LoaderContentRecordType::Program, Title | 0x800, 6,
                        broken_exefs);
        Check(pm.PatchExeFS(empty) == nullptr,
              "present but malformed update ExeFS must not fall back to base");
        manual.ClearAllEntries();
        Check(pm.PatchExeFS(empty, true, broken_exefs, 6) == nullptr,
              "packed malformed update ExeFS must not fall back to base");
    }
    Check(pm.PatchExeFS(empty, true, File(Nca(NcaHeader::ContentType::Meta)), 7) == nullptr,
          "non-program NCA must not be treated as a RomFS-only program update");
    const NCA code_base(good);
    const auto original_data = File({1, 2, 3});
    Check(pm.PatchRomFS(&code_base, original_data, LoaderContentRecordType::Program, good, false,
                        true, 3) == original_data,
          "code-only updates retain base RomFS");
    // Missing Control NCA must reach a controlled loader error rather than dereference null.
    Loader::AppLoader_NSP no_control(File(Nsp(), "base.dnsp"), fsc, providers, 0, 0);
    VirtualFile unused_romfs;
    Check(no_control.ReadRomFS(unused_romfs) == LoaderResultStatus::ErrorNoRomFS,
          "missing Control NCA has a usable secondary loader");
    Loader::AppLoader_XCI no_control_xci(File(Xci(Nsp()), "base.dxci"), fsc, providers, 0, 0);
    Check(no_control_xci.ReadRomFS(unused_romfs) == LoaderResultStatus::ErrorNoRomFS,
          "XCI without Control NCA has a usable loader");
    // A packed, non-BKTR update also needs its CNMT version and correct title category.
    PartitionFilesystem base_files(File(Nsp()));
    Files combined;
    for (const auto &file : base_files.GetFiles())
        combined.emplace_back(file->GetName(), file->ReadAllBytes());
    CNMTHeader update_header{};
    update_header.title_id = Title | 0x800;
    update_header.title_version = 42;
    update_header.type = LoaderTitleType::Update;
    update_header.table_offset = sizeof(OptionalHeader);
    update_header.number_content_entries = 1;
    ContentRecord update_record{};
    update_record.nca_id.fill(0x11);
    update_record.type = LoaderContentRecordType::Program;
    std::vector<u8> update_cnmt;
    Put(update_cnmt, 0, update_header);
    Put(update_cnmt, sizeof(update_header), OptionalHeader{});
    Put(update_cnmt, sizeof(update_header) + sizeof(OptionalHeader), update_record);
    combined.emplace_back(std::string(32, '1') + ".cnmt.nca",
                          Nca(NcaHeader::ContentType::Meta, Pfs({{"update.cnmt", update_cnmt}})));
    combined.emplace_back(std::string(32, '1') + ".nca", good->ReadAllBytes());
    auto packed = File(Pfs(combined), "packed.dnsp");
    Check(NSP(packed).GetTitleVersion(Title | 0x800) == 42, "packed CNMT version retained");
    Loader::AppLoader_NSP packed_nsp(packed, fsc, providers, 0, 0);
    VirtualFile packed_update;
    Check(packed_nsp.ReadUpdateRaw(packed_update) == LoaderResultStatus::Success && packed_update,
          "packed NSP update found");
    Loader::AppLoader_XCI packed_xci(File(Xci(packed->ReadAllBytes()), "packed.dxci"), fsc,
                                     providers, 0, 0);
    packed_update.reset();
    Check(packed_xci.ReadUpdateRaw(packed_update) == LoaderResultStatus::Success && packed_update,
          "packed XCI update found");
    if (argc == 3)
    {
        RealVfsFilesystem vfs;
        NSP base(vfs.OpenFile(argv[1]));
        NSP update(vfs.OpenFile(argv[2]));
        auto tid = base.GetProgramTitleID();
        auto base_nca = base.GetNCA(tid, LoaderContentRecordType::Program);
        auto update_file = update.GetNCAFile(tid | 0x800, LoaderContentRecordType::Program,
                                             LoaderTitleType::Update);
        auto version = update.GetTitleVersion(tid | 0x800);
        Check(base_nca && update_file, "local fixtures found");
        ContentProviderUnion uninstalled;
        RomFSFactory factory(base_nca->RomFS(), true, uninstalled, fsc, nullptr, base_nca);
        factory.SetPackedUpdate(update_file, version);
        Check(factory.OpenCurrentProcess(tid) != nullptr,
              "BKTR resolves with source base absent from installed provider");
        PatchManager local(tid, fsc, uninstalled);
        Check(local.PatchExeFS(nullptr, true, update_file, version) != nullptr,
              "local update ExeFS resolves");
        std::puts("PASS: local BKTR and ExeFS without installed base");
    }
    std::puts("PASS: sparse NSP/XCI, malformed headers, missing Control NCA, update selection and "
              "disable flags");
}
