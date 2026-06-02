// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_sys/registered_cache.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/vfs/vfs_concat.h"
#include "core/file_sys/vfs/vfs_ivirtualfile.h"
#include "core/loader/loader.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/hex_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/yuzu_assert.h"
#include <algorithm>
#include <common/sha256.h>
#include <random>
#include <regex>

bool operator<(const ContentProviderEntry & lhs, const ContentProviderEntry & rhs)
{
    return (lhs.titleID < rhs.titleID) || (lhs.titleID == rhs.titleID && lhs.type < rhs.type);
}

bool operator==(const ContentProviderEntry & lhs, const ContentProviderEntry & rhs)
{
    return std::tie(lhs.titleID, lhs.type) == std::tie(rhs.titleID, rhs.type);
}

bool operator!=(const ContentProviderEntry & lhs, const ContentProviderEntry & rhs)
{
    return !operator==(lhs, rhs);
}

namespace FileSys
{

// The size of blocks to use when vfs raw copying into nand.
constexpr size_t VFS_RC_LARGE_COPY_BLOCK = 0x400000;

static bool FollowsTwoDigitDirFormat(std::string_view name)
{
    static const std::regex two_digit_regex("000000[0-9A-F]{2}", std::regex_constants::ECMAScript |
                                                                     std::regex_constants::icase);
    return std::regex_match(name.begin(), name.end(), two_digit_regex);
}

static bool FollowsNcaIdFormat(std::string_view name)
{
    static const std::regex nca_id_regex("[0-9A-F]{32}\\.nca", std::regex_constants::ECMAScript | std::regex_constants::icase);
    static const std::regex nca_id_cnmt_regex("[0-9A-F]{32}\\.cnmt.nca", std::regex_constants::ECMAScript | std::regex_constants::icase);
    return (name.size() == 36 && std::regex_match(name.begin(), name.end(), nca_id_regex)) || (name.size() == 41 && std::regex_match(name.begin(), name.end(), nca_id_cnmt_regex));
}

static std::string GetRelativePathFromNcaID(const std::array<u8, 16> & nca_id, bool second_hex_upper, bool within_two_digit, bool cnmt_suffix)
{
    if (!within_two_digit)
    {
        const auto format_str = fmt::runtime(cnmt_suffix ? "{}.cnmt.nca" : "/{}.nca");
        return fmt::format(format_str, Common::HexToString(nca_id, second_hex_upper));
    }

    const std::string id_bytes(reinterpret_cast<const char *>(nca_id.data()), nca_id.size());
    const std::string hash_hex = sha256(id_bytes);
    const std::vector<u8> hash = Common::HexStringToVector(hash_hex, false);
    if (hash.empty())
    {
        return {};
    }

    const auto format_str = fmt::runtime(cnmt_suffix ? "/000000{:02X}/{}.cnmt.nca" : "/000000{:02X}/{}.nca");
    return fmt::format(format_str, hash[0], Common::HexToString(nca_id, second_hex_upper));
}

static std::string GetCNMTName(LoaderTitleType type, uint64_t title_id)
{
    static constexpr std::array<const char *, 9> TITLE_TYPE_NAMES{
        "SystemProgram",
        "SystemData",
        "SystemUpdate",
        "BootImagePackage",
        "BootImagePackageSafe",
        "Application",
        "Patch",
        "AddOnContent",
        "" ///< Currently unknown 'DeltaTitle'
    };

    auto index = static_cast<std::size_t>(type);
    // If the index is after the jump in LoaderTitleType, subtract it out.
    if (index >= static_cast<std::size_t>(LoaderTitleType::Application))
    {
        index -= static_cast<std::size_t>(LoaderTitleType::Application) -
                 static_cast<std::size_t>(LoaderTitleType::FirmwarePackageB);
    }
    return fmt::format("{}_{:016x}.cnmt", TITLE_TYPE_NAMES[index], title_id);
}

LoaderContentRecordType GetCRTypeFromNCAType(NCAContentType type)
{
    switch (type)
    {
    case NCAContentType::Program:
        // TODO(DarkLordZach): Differentiate between Program and Patch
        return LoaderContentRecordType::Program;
    case NCAContentType::Meta:
        return LoaderContentRecordType::Meta;
    case NCAContentType::Control:
        return LoaderContentRecordType::Control;
    case NCAContentType::Data:
    case NCAContentType::PublicData:
        return LoaderContentRecordType::Data;
    case NCAContentType::Manual:
        // TODO(DarkLordZach): Peek at NCA contents to differentiate Manual and Legal.
        return LoaderContentRecordType::HtmlDocument;
    default:
        ASSERT_MSG(false, "Invalid NCAContentType={:02X}", type);
        return LoaderContentRecordType{};
    }
}

ContentProvider::~ContentProvider() = default;

IFileSysNCA * ContentProvider::GetEntry(uint64_t title_id, LoaderContentRecordType type) const
{
    return GetEntryNCA(title_id, type).release();
}

bool ContentProvider::HasEntry(ContentProviderEntry entry) const
{
    return HasEntry(entry.titleID, entry.type);
}

VirtualFile ContentProvider::GetEntryUnparsed(ContentProviderEntry entry) const
{
    return GetEntryUnparsed(entry.titleID, entry.type);
}

VirtualFile ContentProvider::GetEntryRaw(ContentProviderEntry entry) const
{
    return GetEntryRaw(entry.titleID, entry.type);
}

std::unique_ptr<NCA> ContentProvider::GetEntryNCA(ContentProviderEntry entry) const
{
    return GetEntryNCA(entry.titleID, entry.type);
}

std::vector<ContentProviderEntry> ContentProvider::ListEntries() const
{
    return ListEntriesFilter(std::nullopt, std::nullopt, std::nullopt);
}

PlaceholderCache::PlaceholderCache(VirtualDir dir_) :
    dir(std::move(dir_))
{
}

bool PlaceholderCache::Create(const NcaID & id, uint64_t size) const
{
    const auto path = GetRelativePathFromNcaID(id, false, true, false);

    if (dir->GetFileRelative(path) != nullptr)
    {
        return false;
    }

    UNIMPLEMENTED();
    const auto dirname = fmt::format("000000{:02X}", 0);

    const auto dir2 = GetOrCreateDirectoryRelative(dir, dirname);

    if (dir2 == nullptr)
        return false;

    const auto file = dir2->CreateFile(fmt::format("{}.nca", Common::HexToString(id, false)));

    if (file == nullptr)
        return false;

    return file->Resize(size);
}

bool PlaceholderCache::Delete(const NcaID & id) const
{
    const auto path = GetRelativePathFromNcaID(id, false, true, false);

    if (dir->GetFileRelative(path) == nullptr)
    {
        return false;
    }

    UNIMPLEMENTED();
    const auto dirname = fmt::format("000000{:02X}", 0);

    const auto dir2 = GetOrCreateDirectoryRelative(dir, dirname);

    const auto res = dir2->DeleteFile(fmt::format("{}.nca", Common::HexToString(id, false)));

    return res;
}

bool PlaceholderCache::Exists(const NcaID & id) const
{
    const auto path = GetRelativePathFromNcaID(id, false, true, false);

    return dir->GetFileRelative(path) != nullptr;
}

bool PlaceholderCache::Write(const NcaID & id, uint64_t offset, const std::vector<u8> & data) const
{
    const auto path = GetRelativePathFromNcaID(id, false, true, false);
    const auto file = dir->GetFileRelative(path);

    if (file == nullptr)
        return false;

    return file->WriteBytes(data, offset) == data.size();
}

bool PlaceholderCache::Register(RegisteredCache * cache, const NcaID & placeholder,
                                const NcaID & install) const
{
    const auto path = GetRelativePathFromNcaID(placeholder, false, true, false);
    const auto file = dir->GetFileRelative(path);

    if (file == nullptr)
        return false;

    const auto res = cache->RawInstallNCA(NCA{file}, &VfsRawCopy, false, install);

    if (res != InstallResult::Success)
        return false;

    return Delete(placeholder);
}

bool PlaceholderCache::CleanAll() const
{
    return dir->GetParentDirectory()->CleanSubdirectoryRecursive(dir->GetName());
}

std::optional<std::array<u8, 0x10>> PlaceholderCache::GetRightsID(const NcaID & id) const
{
    const auto path = GetRelativePathFromNcaID(id, false, true, false);
    const auto file = dir->GetFileRelative(path);

    if (file == nullptr)
        return std::nullopt;

    NCA nca{file};

    if (nca.GetStatus() != LoaderResultStatus::Success &&
        nca.GetStatus() != LoaderResultStatus::ErrorMissingBKTRBaseRomFS)
    {
        return std::nullopt;
    }

    const auto rights_id = nca.GetRightsId();
    if (rights_id == NcaID{})
        return std::nullopt;

    return rights_id;
}

uint64_t PlaceholderCache::Size(const NcaID & id) const
{
    const auto path = GetRelativePathFromNcaID(id, false, true, false);
    const auto file = dir->GetFileRelative(path);

    if (file == nullptr)
        return 0;

    return file->GetSize();
}

bool PlaceholderCache::SetSize(const NcaID & id, uint64_t new_size) const
{
    const auto path = GetRelativePathFromNcaID(id, false, true, false);
    const auto file = dir->GetFileRelative(path);

    if (file == nullptr)
        return false;

    return file->Resize(new_size);
}

std::vector<NcaID> PlaceholderCache::List() const
{
    std::vector<NcaID> out;
    for (const auto & sdir : dir->GetSubdirectories())
    {
        for (const auto & file : sdir->GetFiles())
        {
            const auto name = file->GetName();
            if (name.length() == 36 && name.ends_with(".nca"))
            {
                out.push_back(Common::HexStringToArray<0x10>(name.substr(0, 32)));
            }
        }
    }
    return out;
}

NcaID PlaceholderCache::Generate()
{
    std::random_device device;
    std::mt19937 gen(device());
    std::uniform_int_distribution<uint64_t> distribution(1, std::numeric_limits<uint64_t>::max());

    NcaID out{};

    const auto v1 = distribution(gen);
    const auto v2 = distribution(gen);
    std::memcpy(out.data(), &v1, sizeof(uint64_t));
    std::memcpy(out.data() + sizeof(uint64_t), &v2, sizeof(uint64_t));

    return out;
}

VirtualFile RegisteredCache::OpenFileOrDirectoryConcat(const VirtualDir & open_dir,
                                                       std::string_view path) const
{
    const auto file = open_dir->GetFileRelative(path);
    if (file != nullptr)
    {
        return file;
    }

    const auto nca_dir = open_dir->GetDirectoryRelative(path);
    if (nca_dir == nullptr)
    {
        return nullptr;
    }

    const auto files = nca_dir->GetFiles();
    if (files.size() == 1 && files[0]->GetName() == "00")
    {
        return files[0];
    }

    std::vector<VirtualFile> concat;
    // Since the files are a two-digit hex number, max is FF.
    for (std::size_t i = 0; i < 0x100; ++i)
    {
        auto next = nca_dir->GetFile(fmt::format("{:02X}", i));
        if (next != nullptr)
        {
            concat.push_back(std::move(next));
        }
        else
        {
            next = nca_dir->GetFile(fmt::format("{:02x}", i));
            if (next != nullptr)
            {
                concat.push_back(std::move(next));
            }
            else
            {
                break;
            }
        }
    }

    if (concat.empty())
    {
        return nullptr;
    }

    auto name = concat.front()->GetName();
    return ConcatenatedVfsFile::MakeConcatenatedFile(std::move(name), std::move(concat));
}

VirtualFile RegisteredCache::GetFileAtID(NcaID id) const
{
    VirtualFile file;
    // Try all five relevant modes of file storage:
    // (bit 2 = uppercase/lower, bit 1 = within a two-digit dir, bit 0 = .cnmt suffix)
    // 000: /000000**/{:032X}.nca
    // 010: /{:032X}.nca
    // 100: /000000**/{:032x}.nca
    // 110: /{:032x}.nca
    // 111: /{:032x}.cnmt.nca
    for (u8 i = 0; i < 8; ++i)
    {
        if ((i % 2) == 1 && i != 7)
            continue;
        const auto path =
            GetRelativePathFromNcaID(id, (i & 0b100) == 0, (i & 0b010) == 0, (i & 0b001) == 0b001);
        file = OpenFileOrDirectoryConcat(dir, path);
        if (file != nullptr)
            return file;
    }
    return file;
}

static std::optional<NcaID> CheckMapForContentRecord(const std::map<uint64_t, CNMT> & map, uint64_t title_id,
                                                     LoaderContentRecordType type)
{
    const auto cmnt_iter = map.find(title_id);
    if (cmnt_iter == map.cend())
    {
        return std::nullopt;
    }

    const auto & cnmt = cmnt_iter->second;
    const auto & content_records = cnmt.GetContentRecords();
    const auto iter = std::find_if(content_records.cbegin(), content_records.cend(),
                                   [type](const ContentRecord & rec) { return rec.type == type; });
    if (iter == content_records.cend())
    {
        return std::nullopt;
    }

    return std::make_optional(iter->nca_id);
}

std::optional<NcaID> RegisteredCache::GetNcaIDFromMetadata(uint64_t title_id,
                                                           LoaderContentRecordType type) const
{
    if (type == LoaderContentRecordType::Meta && meta_id.find(title_id) != meta_id.end())
        return meta_id.at(title_id);

    const auto res1 = CheckMapForContentRecord(yuzu_meta, title_id, type);
    if (res1)
        return res1;
    return CheckMapForContentRecord(meta, title_id, type);
}

std::vector<NcaID> RegisteredCache::AccumulateFiles() const
{
    std::vector<NcaID> ids;
    for (const auto & d2_dir : dir->GetSubdirectories())
    {
        if (FollowsNcaIdFormat(d2_dir->GetName()))
        {
            ids.push_back(Common::HexStringToArray<0x10, true>(d2_dir->GetName().substr(0, 0x20)));
            continue;
        }

        if (!FollowsTwoDigitDirFormat(d2_dir->GetName()))
            continue;

        for (const auto & nca_dir : d2_dir->GetSubdirectories())
        {
            if (nca_dir == nullptr || !FollowsNcaIdFormat(nca_dir->GetName()))
            {
                continue;
            }

            ids.push_back(Common::HexStringToArray<0x10, true>(nca_dir->GetName().substr(0, 0x20)));
        }

        for (const auto & nca_file : d2_dir->GetFiles())
        {
            if (nca_file == nullptr || !FollowsNcaIdFormat(nca_file->GetName()))
            {
                continue;
            }

            ids.push_back(
                Common::HexStringToArray<0x10, true>(nca_file->GetName().substr(0, 0x20)));
        }
    }

    for (const auto & d2_file : dir->GetFiles())
    {
        if (FollowsNcaIdFormat(d2_file->GetName()))
            ids.push_back(Common::HexStringToArray<0x10, true>(d2_file->GetName().substr(0, 0x20)));
    }
    return ids;
}

void RegisteredCache::ProcessFiles(const std::vector<NcaID> & ids)
{
    for (const auto & id : ids)
    {
        const auto file = GetFileAtID(id);

        if (file == nullptr)
        {
            continue;
        }
        const auto nca = std::make_shared<NCA>(parser(file, id));
        if (nca->GetStatus() != LoaderResultStatus::Success || nca->GetType() != NCAContentType::Meta || nca->GetSubdirectories().empty())
        {
            continue;
        }

        const auto section0 = nca->GetSubdirectories()[0];

        for (const auto & section0_file : section0->GetFiles())
        {
            if (section0_file->GetExtension() != "cnmt")
            {
                continue;
            }
            meta.insert_or_assign(nca->GetTitleId(), CNMT(section0_file));
            meta_id.insert_or_assign(nca->GetTitleId(), id);
            break;
        }
    }
}

void RegisteredCache::AccumulateYuzuMeta()
{
    const auto meta_dir = dir->GetSubdirectory("yuzu_meta");
    if (meta_dir == nullptr)
    {
        return;
    }

    for (const auto & file : meta_dir->GetFiles())
    {
        if (file->GetExtension() != "cnmt")
        {
            continue;
        }

        CNMT cnmt(file);
        yuzu_meta.insert_or_assign(cnmt.GetTitleID(), std::move(cnmt));
    }
}

void RegisteredCache::Refresh()
{
    if (dir == nullptr)
    {
        return;
    }

    const auto ids = AccumulateFiles();
    ProcessFiles(ids);
    AccumulateYuzuMeta();
}

RegisteredCache::RegisteredCache(VirtualDir dir_, ContentProviderParsingFunction parsing_function) :
    dir(std::move(dir_)),
    parser(std::move(parsing_function))
{
    Refresh();
}

RegisteredCache::~RegisteredCache() = default;

bool RegisteredCache::HasEntry(uint64_t title_id, LoaderContentRecordType type) const
{
    return GetEntryRaw(title_id, type) != nullptr;
}

VirtualFile RegisteredCache::GetEntryUnparsed(uint64_t title_id, LoaderContentRecordType type) const
{
    const auto id = GetNcaIDFromMetadata(title_id, type);
    return id ? GetFileAtID(*id) : nullptr;
}

std::optional<u32> RegisteredCache::GetEntryVersion(uint64_t title_id) const
{
    const auto meta_iter = meta.find(title_id);
    if (meta_iter != meta.cend())
    {
        return meta_iter->second.GetTitleVersion();
    }

    const auto yuzu_meta_iter = yuzu_meta.find(title_id);
    if (yuzu_meta_iter != yuzu_meta.cend())
    {
        return yuzu_meta_iter->second.GetTitleVersion();
    }

    return std::nullopt;
}

VirtualFile RegisteredCache::GetEntryRaw(uint64_t title_id, LoaderContentRecordType type) const
{
    const auto id = GetNcaIDFromMetadata(title_id, type);
    return id ? parser(GetFileAtID(*id), *id) : nullptr;
}

IFileSysNCA * RegisteredCache::GetEntry(uint64_t title_id, LoaderContentRecordType type) const
{
    return GetEntryNCA(title_id, type).release();
}

std::unique_ptr<NCA> RegisteredCache::GetEntryNCA(uint64_t title_id, LoaderContentRecordType type) const
{
    const auto raw = GetEntryRaw(title_id, type);
    if (raw == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<NCA>(raw);
}

template <typename T>
void RegisteredCache::IterateAllMetadata(std::vector<T> & out, std::function<T(const CNMT &, const ContentRecord &)> proc, std::function<bool(const CNMT &, const ContentRecord &)> filter) const
{
    for (const auto & kv : meta)
    {
        const auto & cnmt = kv.second;
        if (filter(cnmt, EMPTY_META_CONTENT_RECORD))
        {
            out.push_back(proc(cnmt, EMPTY_META_CONTENT_RECORD));
        }

        for (const auto & rec : cnmt.GetContentRecords())
        {
            if (GetFileAtID(rec.nca_id) != nullptr && filter(cnmt, rec))
            {
                out.push_back(proc(cnmt, rec));
            }
        }
    }
    for (const auto & kv : yuzu_meta)
    {
        const auto & cnmt = kv.second;
        for (const auto & rec : cnmt.GetContentRecords())
        {
            if (GetFileAtID(rec.nca_id) != nullptr && filter(cnmt, rec))
            {
                out.push_back(proc(cnmt, rec));
            }
        }
    }
}

std::vector<ContentProviderEntry> RegisteredCache::ListEntriesFilter(std::optional<LoaderTitleType> title_type, std::optional<LoaderContentRecordType> record_type, std::optional<uint64_t> title_id) const
{
    std::vector<ContentProviderEntry> out;
    IterateAllMetadata<ContentProviderEntry>(
        out,
        [](const CNMT & c, const ContentRecord & r) {
            return ContentProviderEntry{c.GetTitleID(), r.type};
        },
        [&title_type, &record_type, &title_id](const CNMT & c, const ContentRecord & r) {
            if (title_type && *title_type != c.GetType())
                return false;
            if (record_type && *record_type != r.type)
                return false;
            if (title_id && *title_id != c.GetTitleID())
                return false;
            return true;
        });
    return out;
}

static std::shared_ptr<NCA> GetNCAFromNSPForID(const NSP & nsp, const NcaID & id)
{
    UNIMPLEMENTED();
    return nullptr;
}

InstallResult RegisteredCache::InstallEntry(const XCI & xci, bool overwrite_if_exists,
                                            const VfsCopyFunction & copy)
{
    UNIMPLEMENTED();
    return InstallResult::ErrorMetaFailed;
}

InstallResult RegisteredCache::InstallEntry(const NSP & nsp, bool overwrite_if_exists,
                                            const VfsCopyFunction & copy)
{
    UNIMPLEMENTED();
    return InstallResult::ErrorMetaFailed;
}

InstallResult RegisteredCache::InstallEntry(const NCA & nca, LoaderTitleType type,
                                            bool overwrite_if_exists, const VfsCopyFunction & copy)
{
    const CNMTHeader header{
        .title_id = nca.GetTitleId(),
        .title_version = 0,
        .type = type,
        .reserved = {},
        .table_offset = 0x10,
        .number_content_entries = 1,
        .number_meta_entries = 0,
        .attributes = 0,
        .reserved2 = {},
        .is_committed = 0,
        .required_download_system_version = 0,
        .reserved3 = {},
    };
    const OptionalHeader opt_header{0, 0};
    ContentRecord c_rec{{}, {}, {}, GetCRTypeFromNCAType(nca.GetType()), {}};
    const auto & data = nca.BaseFile()->ReadBytes(0x100000);
    UNIMPLEMENTED();
    std::memcpy(&c_rec.nca_id, &c_rec.hash, 16);
    const CNMT new_cnmt(header, opt_header, {c_rec}, {});
    if (!RawInstallYuzuMeta(new_cnmt))
    {
        return InstallResult::ErrorMetaFailed;
    }
    return RawInstallNCA(nca, copy, overwrite_if_exists, c_rec.nca_id);
}

InstallResult RegisteredCache::InstallEntry(const NCA & nca, const CNMTHeader & base_header,
                                            const ContentRecord & base_record,
                                            bool overwrite_if_exists, const VfsCopyFunction & copy)
{
    const CNMTHeader header{
        .title_id = nca.GetTitleId(),
        .title_version = base_header.title_version,
        .type = base_header.type,
        .reserved = {},
        .table_offset = 0x10,
        .number_content_entries = 1,
        .number_meta_entries = 0,
        .attributes = 0,
        .reserved2 = {},
        .is_committed = 0,
        .required_download_system_version = 0,
        .reserved3 = {},
    };
    const OptionalHeader opt_header{0, 0};
    const CNMT new_cnmt(header, opt_header, {base_record}, {});
    if (!RawInstallYuzuMeta(new_cnmt))
    {
        return InstallResult::ErrorMetaFailed;
    }
    return RawInstallNCA(nca, copy, overwrite_if_exists, base_record.nca_id);
}

bool RegisteredCache::RemoveExistingEntry(uint64_t title_id) const
{
    bool removed_data = false;

    const auto delete_nca = [this](const NcaID & id) {
        const auto path = GetRelativePathFromNcaID(id, false, true, false);

        const bool isFile = dir->GetFileRelative(path) != nullptr;
        const bool isDir = dir->GetDirectoryRelative(path) != nullptr;

        if (isFile)
        {
            return dir->DeleteFile(path);
        }
        else if (isDir)
        {
            return dir->DeleteSubdirectoryRecursive(path);
        }

        return false;
    };

    // If an entry exists in the registered cache, remove it
    if (HasEntry(title_id, LoaderContentRecordType::Meta))
    {
        LOG_INFO(Loader,
                 "Previously installed entry (v{}) for title_id={:016X} detected! "
                 "Attempting to remove...",
                 GetEntryVersion(title_id).value_or(0), title_id);

        // Get all the ncas associated with the current CNMT and delete them
        const auto meta_old_id =
            GetNcaIDFromMetadata(title_id, LoaderContentRecordType::Meta).value_or(NcaID{});
        const auto program_id =
            GetNcaIDFromMetadata(title_id, LoaderContentRecordType::Program).value_or(NcaID{});
        const auto data_id =
            GetNcaIDFromMetadata(title_id, LoaderContentRecordType::Data).value_or(NcaID{});
        const auto control_id =
            GetNcaIDFromMetadata(title_id, LoaderContentRecordType::Control).value_or(NcaID{});
        const auto html_id =
            GetNcaIDFromMetadata(title_id, LoaderContentRecordType::HtmlDocument).value_or(NcaID{});
        const auto legal_id =
            GetNcaIDFromMetadata(title_id, LoaderContentRecordType::LegalInformation).value_or(NcaID{});

        const auto deleted_meta = delete_nca(meta_old_id);
        const auto deleted_program = delete_nca(program_id);
        const auto deleted_data = delete_nca(data_id);
        const auto deleted_control = delete_nca(control_id);
        const auto deleted_html = delete_nca(html_id);
        const auto deleted_legal = delete_nca(legal_id);

        removed_data |= (deleted_meta || deleted_program || deleted_data || deleted_control ||
                         deleted_html || deleted_legal);
    }

    // If patch entries for any program exist in yuzu meta, remove them
    for (u8 i = 0; i < 0x10; i++)
    {
        const auto meta_dir = dir->CreateDirectoryRelative("yuzu_meta");
        const auto filename = GetCNMTName(LoaderTitleType::Update, title_id + i);
        if (meta_dir->GetFile(filename))
        {
            removed_data |= meta_dir->DeleteFile(filename);
        }
    }

    return removed_data;
}

InstallResult RegisteredCache::RawInstallNCA(const NCA & nca, const VfsCopyFunction & copy, bool overwrite_if_exists, std::optional<NcaID> override_id)
{
    const auto in = nca.BaseFile();
    UNIMPLEMENTED();

    // Calculate NcaID
    // NOTE: Because computing the SHA256 of an entire NCA is quite expensive (especially if the
    // game is massive), we're going to cheat and only hash the first MB of the NCA.
    // Also, for XCIs the NcaID matters, so if the override id isn't none, use that.
    NcaID id{};
    if (override_id)
    {
        id = *override_id;
    }
    else
    {
        const auto & data = in->ReadBytes(0x100000);
        UNIMPLEMENTED();
    }

    std::string path = GetRelativePathFromNcaID(id, false, true, false);

    if (GetFileAtID(id) != nullptr && !overwrite_if_exists)
    {
        LOG_WARNING(Loader, "Attempting to overwrite existing NCA. Skipping...");
        return InstallResult::ErrorAlreadyExists;
    }

    if (GetFileAtID(id) != nullptr)
    {
        LOG_WARNING(Loader, "Overwriting existing NCA...");
        VirtualDir c_dir;
        {
            c_dir = dir->GetFileRelative(path)->GetContainingDirectory();
        }
        c_dir->DeleteFile(Common::FS::GetFilename(path));
    }

    auto out = dir->CreateFileRelative(path);
    if (out == nullptr)
    {
        return InstallResult::ErrorCopyFailed;
    }
    return copy(in, out, VFS_RC_LARGE_COPY_BLOCK) ? InstallResult::Success
                                                  : InstallResult::ErrorCopyFailed;
}

bool RegisteredCache::RawInstallYuzuMeta(const CNMT & cnmt)
{
    // Reasoning behind this method can be found in the comment for InstallEntry, NCA overload.
    const auto meta_dir = dir->CreateDirectoryRelative("yuzu_meta");
    const auto filename = GetCNMTName(cnmt.GetType(), cnmt.GetTitleID());
    if (meta_dir->GetFile(filename) == nullptr)
    {
        auto out = meta_dir->CreateFile(filename);
        const auto buffer = cnmt.Serialize();
        out->Resize(buffer.size());
        out->WriteBytes(buffer);
    }
    else
    {
        auto out = meta_dir->GetFile(filename);
        CNMT old_cnmt(out);
        // Returns true on change
        if (old_cnmt.UnionRecords(cnmt))
        {
            out->Resize(0);
            const auto buffer = old_cnmt.Serialize();
            out->Resize(buffer.size());
            out->WriteBytes(buffer);
        }
    }
    Refresh();
    return std::find_if(yuzu_meta.begin(), yuzu_meta.end(),
                        [&cnmt](const std::pair<uint64_t, CNMT> & kv) {
                            return kv.second.GetType() == cnmt.GetType() &&
                                   kv.second.GetTitleID() == cnmt.GetTitleID();
                        }) != yuzu_meta.end();
}

ContentProviderUnion::~ContentProviderUnion() = default;

void ContentProviderUnion::SetSlot(ContentProviderUnionSlot slot, ContentProvider * provider)
{
    providers[slot] = provider;
}

void ContentProviderUnion::ClearSlot(ContentProviderUnionSlot slot)
{
    providers[slot] = nullptr;
}

void ContentProviderUnion::Refresh()
{
    for (auto & provider : providers)
    {
        if (provider.second == nullptr)
        {
            continue;
        }
        provider.second->Refresh();
    }
}

bool ContentProviderUnion::HasEntry(uint64_t title_id, LoaderContentRecordType type) const
{
    for (const auto & provider : providers)
    {
        if (provider.second == nullptr)
            continue;

        if (provider.second->HasEntry(title_id, type))
            return true;
    }

    return false;
}

std::optional<u32> ContentProviderUnion::GetEntryVersion(uint64_t title_id) const
{
    for (const auto & provider : providers)
    {
        if (provider.second == nullptr)
            continue;

        const auto res = provider.second->GetEntryVersion(title_id);
        if (res != std::nullopt)
            return res;
    }

    return std::nullopt;
}

VirtualFile ContentProviderUnion::GetEntryUnparsed(uint64_t title_id, LoaderContentRecordType type) const
{
    for (const auto & provider : providers)
    {
        if (provider.second == nullptr)
            continue;

        const auto res = provider.second->GetEntryUnparsed(title_id, type);
        if (res != nullptr)
        {
            return res;
        }
    }

    return nullptr;
}

VirtualFile ContentProviderUnion::GetEntryRaw(uint64_t title_id, LoaderContentRecordType type) const
{
    for (const auto & provider : providers)
    {
        if (provider.second == nullptr)
        {
            continue;
        }

        const auto res = provider.second->GetEntryRaw(title_id, type);
        if (res != nullptr)
        {
            return res;
        }
    }

    return nullptr;
}

std::unique_ptr<NCA> ContentProviderUnion::GetEntryNCA(uint64_t title_id, LoaderContentRecordType type) const
{
    for (const auto & provider : providers)
    {
        if (provider.second == nullptr)
            continue;

        auto res = provider.second->GetEntryNCA(title_id, type);
        if (res != nullptr)
            return res;
    }

    return nullptr;
}

std::vector<ContentProviderEntry> ContentProviderUnion::ListEntriesFilter(std::optional<LoaderTitleType> title_type, std::optional<LoaderContentRecordType> record_type, std::optional<uint64_t> title_id) const
{
    std::vector<ContentProviderEntry> out;

    for (const auto & provider : providers)
    {
        if (provider.second == nullptr)
            continue;

        const auto vec = provider.second->ListEntriesFilter(title_type, record_type, title_id);
        std::copy(vec.begin(), vec.end(), std::back_inserter(out));
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::pair<ContentProviderUnionSlot, ContentProviderEntry>>
ContentProviderUnion::ListEntriesFilterOrigin(std::optional<ContentProviderUnionSlot> origin,
                                              std::optional<LoaderTitleType> title_type,
                                              std::optional<LoaderContentRecordType> record_type,
                                              std::optional<uint64_t> title_id) const
{
    std::vector<std::pair<ContentProviderUnionSlot, ContentProviderEntry>> out;

    for (const auto & provider : providers)
    {
        if (provider.second == nullptr)
            continue;

        if (origin.has_value() && *origin != provider.first)
            continue;

        const auto vec = provider.second->ListEntriesFilter(title_type, record_type, title_id);
        std::transform(vec.begin(), vec.end(), std::back_inserter(out),
                       [&provider](const ContentProviderEntry & entry) {
                           return std::make_pair(provider.first, entry);
                       });
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::optional<ContentProviderUnionSlot> ContentProviderUnion::GetSlotForEntry(uint64_t title_id, LoaderContentRecordType type) const
{
    const auto iter =
        std::find_if(providers.begin(), providers.end(), [title_id, type](const auto & provider) {
            return provider.second != nullptr && provider.second->HasEntry(title_id, type);
        });

    if (iter == providers.end())
    {
        return std::nullopt;
    }

    return iter->first;
}

ManualContentProvider::~ManualContentProvider() = default;

void ManualContentProvider::AddEntry(LoaderTitleType title_type, LoaderContentRecordType content_type, uint64_t title_id, VirtualFile file)
{
    entries.insert_or_assign({title_type, content_type, title_id}, file);
}

void ManualContentProvider::ClearAllEntries()
{
    entries.clear();
}

void ManualContentProvider::Refresh()
{
}

bool ManualContentProvider::HasEntry(uint64_t title_id, LoaderContentRecordType type) const
{
    return GetEntryRaw(title_id, type) != nullptr;
}

std::optional<u32> ManualContentProvider::GetEntryVersion(uint64_t title_id) const
{
    return std::nullopt;
}

VirtualFile ManualContentProvider::GetEntryUnparsed(uint64_t title_id, LoaderContentRecordType type) const
{
    return GetEntryRaw(title_id, type);
}

VirtualFile ManualContentProvider::GetEntryRaw(uint64_t title_id, LoaderContentRecordType type) const
{
    const auto iter =
        std::find_if(entries.begin(), entries.end(), [title_id, type](const auto & entry) {
            const auto content_type = std::get<1>(entry.first);
            const auto e_title_id = std::get<2>(entry.first);
            return content_type == type && e_title_id == title_id;
        });
    if (iter == entries.end())
        return nullptr;
    return iter->second;
}

std::unique_ptr<NCA> ManualContentProvider::GetEntryNCA(uint64_t title_id, LoaderContentRecordType type) const
{
    const auto res = GetEntryRaw(title_id, type);
    if (res == nullptr)
        return nullptr;
    return std::make_unique<NCA>(res);
}

std::vector<ContentProviderEntry> ManualContentProvider::ListEntriesFilter(
    std::optional<LoaderTitleType> title_type, std::optional<LoaderContentRecordType> record_type,
    std::optional<uint64_t> title_id) const
{
    std::vector<ContentProviderEntry> out;

    for (const auto & entry : entries)
    {
        const auto [e_title_type, e_content_type, e_title_id] = entry.first;
        if ((title_type == std::nullopt || e_title_type == *title_type) &&
            (record_type == std::nullopt || e_content_type == *record_type) &&
            (title_id == std::nullopt || e_title_id == *title_id))
        {
            out.emplace_back(ContentProviderEntry{e_title_id, e_content_type});
        }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
} // namespace FileSys

void ManualContentProviderImpl::AddEntry(LoaderTitleType title_type, LoaderContentRecordType content_type, uint64_t title_id, IVirtualFile * file)
{
    provider.AddEntry(title_type, content_type, title_id, std::make_shared<VfsVirtualFile>(file));
}

void ManualContentProviderImpl::ClearAllEntries()
{
    provider.ClearAllEntries();
}

FileSys::ManualContentProvider & ManualContentProviderImpl::Provider()
{
    return provider;
}
