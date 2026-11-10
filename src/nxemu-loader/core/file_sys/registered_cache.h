// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/file_sys/vfs/vfs.h"
#include "yuzu_common/common_types.h"
#include <array>
#include <boost/container/flat_map.hpp>
#include <functional>
#include <map>
#include <memory>
#include <nxemu-module-spec/system_loader.h>
#include <string>
#include <vector>

namespace FileSys
{
class CNMT;
class NCA;
class NSP;
class XCI;

enum class NCAContentType : u8;

struct ContentRecord;
struct CNMTHeader;
struct MetaRecord;
class RegisteredCache;

using NcaID = std::array<u8, 0x10>;
using ContentProviderParsingFunction = std::function<VirtualFile(const VirtualFile &, const NcaID &)>;
using VfsCopyFunction = std::function<bool(const VirtualFile &, const VirtualFile &, size_t)>;

enum class InstallResult
{
    Success,
    OverwriteExisting,
    ErrorAlreadyExists,
    ErrorCopyFailed,
    ErrorMetaFailed,
    ErrorBaseInstall,
};

constexpr uint64_t GetUpdateTitleID(uint64_t base_title_id)
{
    return base_title_id | 0x800;
}

LoaderContentRecordType GetCRTypeFromNCAType(NCAContentType type);

// boost flat_map requires operator< for O(log(n)) lookups.
bool operator<(const ContentProviderEntry & lhs, const ContentProviderEntry & rhs);

// std unique requires operator== to identify duplicates.
bool operator==(const ContentProviderEntry & lhs, const ContentProviderEntry & rhs);
bool operator!=(const ContentProviderEntry & lhs, const ContentProviderEntry & rhs);

class ManualContentProvider;

class ContentProvider :
    public IContentProvider
{
public:
    virtual ~ContentProvider();

    // IContentProvider
    IFileSysNCA * GetEntry(uint64_t title_id, LoaderContentRecordType type) const override;

    virtual void Refresh() = 0;

    virtual bool HasEntry(uint64_t title_id, LoaderContentRecordType type) const = 0;
    bool HasEntry(ContentProviderEntry entry) const;

    virtual std::optional<u32> GetEntryVersion(uint64_t title_id) const = 0;

    virtual VirtualFile GetEntryUnparsed(uint64_t title_id, LoaderContentRecordType type) const = 0;
    VirtualFile GetEntryUnparsed(ContentProviderEntry entry) const;

    virtual VirtualFile GetEntryRaw(uint64_t title_id, LoaderContentRecordType type) const = 0;
    VirtualFile GetEntryRaw(ContentProviderEntry entry) const;

    virtual std::unique_ptr<NCA> GetEntryNCA(uint64_t title_id, LoaderContentRecordType type) const = 0;
    std::unique_ptr<NCA> GetEntryNCA(ContentProviderEntry entry) const;

    virtual std::vector<ContentProviderEntry> ListEntries() const;

    // If a parameter is not std::nullopt, it will be filtered for from all entries.
    virtual std::vector<ContentProviderEntry> ListEntriesFilter(
        std::optional<LoaderTitleType> title_type = {}, std::optional<LoaderContentRecordType> record_type = {},
        std::optional<uint64_t> title_id = {}) const = 0;

    virtual const ManualContentProvider * GetManualContentProvider() const;
};

class PlaceholderCache
{
public:
    explicit PlaceholderCache(VirtualDir dir);

    bool Create(const NcaID & id, uint64_t size) const;
    bool Delete(const NcaID & id) const;
    bool Exists(const NcaID & id) const;
    bool Write(const NcaID & id, uint64_t offset, const std::vector<u8> & data) const;
    bool Register(RegisteredCache * cache, const NcaID & placeholder, const NcaID & install) const;
    bool CleanAll() const;
    std::optional<std::array<u8, 0x10>> GetRightsID(const NcaID & id) const;
    uint64_t Size(const NcaID & id) const;
    bool SetSize(const NcaID & id, uint64_t new_size) const;
    std::vector<NcaID> List() const;

    static NcaID Generate();

private:
    VirtualDir dir;
};

/*
 * A class that catalogues NCAs in the registered directory structure.
 * Nintendo's registered format follows this structure:
 *
 * Root
 *   | 000000XX <- XX is the ____ two digits of the NcaID
 *       | <hash>.nca <- hash is the NcaID (first half of SHA256 over entire file) (folder)
 *         | 00
 *         | 01 <- Actual content split along 4GB boundaries. (optional)
 *
 * (This impl also supports substituting the nca dir for an nca file, as that's more convenient
 * when 4GB splitting can be ignored.)
 */
class RegisteredCache :
    public IFileSysRegisteredCache,
    public ContentProvider
{
    friend class PlaceholderCache;

public:
    // Parsing function defines the conversion from raw file to NCA. If there are other steps
    // besides creating the NCA from the file (e.g. NAX0 on SD Card), that should go in a custom
    // parsing function.
    explicit RegisteredCache(VirtualDir dir, ContentProviderParsingFunction parsing_function = [](const VirtualFile & file, const NcaID & id) { return file; });
    ~RegisteredCache() override;

    // IFileSysRegisteredCache
    IFileSysNCA * GetEntry(uint64_t title_id, LoaderContentRecordType type) const override;

    void Refresh() override;

    bool HasEntry(uint64_t title_id, LoaderContentRecordType type) const override;

    std::optional<u32> GetEntryVersion(uint64_t title_id) const override;

    VirtualFile GetEntryUnparsed(uint64_t title_id, LoaderContentRecordType type) const override;

    VirtualFile GetEntryRaw(uint64_t title_id, LoaderContentRecordType type) const override;

    std::unique_ptr<NCA> GetEntryNCA(uint64_t title_id, LoaderContentRecordType type) const override;

    // If a parameter is not std::nullopt, it will be filtered for from all entries.
    std::vector<ContentProviderEntry> ListEntriesFilter(std::optional<LoaderTitleType> title_type = {}, std::optional<LoaderContentRecordType> record_type = {}, std::optional<uint64_t> title_id = {}) const override;

    // Raw copies all the ncas from the xci/nsp to the csache. Does some quick checks to make sure
    // there is a meta NCA and all of them are accessible.
    InstallResult InstallEntry(const XCI & xci, bool overwrite_if_exists = false, const VfsCopyFunction & copy = &VfsRawCopy);
    InstallResult InstallEntry(const NSP & nsp, bool overwrite_if_exists = false, const VfsCopyFunction & copy = &VfsRawCopy);

    // Due to the fact that we must use Meta-type NCAs to determine the existence of files, this
    // poses quite a challenge. Instead of creating a new meta NCA for this file, yuzu will create a
    // dir inside the NAND called 'yuzu_meta' and store the raw CNMT there.
    // TODO(DarkLordZach): Author real meta-type NCAs and install those.
    InstallResult InstallEntry(const NCA & nca, LoaderTitleType type, bool overwrite_if_exists = false, const VfsCopyFunction & copy = &VfsRawCopy);
    InstallResult InstallEntry(const NCA & nca, const CNMTHeader & base_header, const ContentRecord & base_record, bool overwrite_if_exists = false, const VfsCopyFunction & copy = &VfsRawCopy);

    // Removes an existing entry based on title id
    bool RemoveExistingEntry(uint64_t title_id) const;

private:
    template <typename T>
    void IterateAllMetadata(std::vector<T> & out, std::function<T(const CNMT &, const ContentRecord &)> proc, std::function<bool(const CNMT &, const ContentRecord &)> filter) const;
    std::vector<NcaID> AccumulateFiles() const;
    void ProcessFiles(const std::vector<NcaID> & ids);
    void AccumulateYuzuMeta();
    std::optional<NcaID> GetNcaIDFromMetadata(uint64_t title_id, LoaderContentRecordType type) const;
    VirtualFile GetFileAtID(NcaID id) const;
    VirtualFile OpenFileOrDirectoryConcat(const VirtualDir & open_dir, std::string_view path) const;
    InstallResult RawInstallNCA(const NCA & nca, const VfsCopyFunction & copy, bool overwrite_if_exists, std::optional<NcaID> override_id = {});
    bool RawInstallYuzuMeta(const CNMT & cnmt);

    VirtualDir dir;
    ContentProviderParsingFunction parser;

    // maps tid -> NcaID of meta
    std::map<uint64_t, NcaID> meta_id;
    // maps tid -> meta
    std::map<uint64_t, CNMT> meta;
    // maps tid -> meta for CNMT in yuzu_meta
    std::map<uint64_t, CNMT> yuzu_meta;
};

enum class ContentProviderUnionSlot
{
    SysNAND,        ///< System NAND
    UserNAND,       ///< User NAND
    SDMC,           ///< SD Card
    FrontendManual, ///< Frontend-defined game list or similar
};

struct ManualUpdateEntry
{
    static constexpr size_t FileSlotCount = (size_t)LoaderContentRecordType::DeltaFragment + 1;

    uint64_t title_id{};
    u32 version{};
    std::array<VirtualFile, FileSlotCount> files;
};

// Combines multiple ContentProvider(s) (i.e. SysNAND, UserNAND, SDMC) into one interface.
class ContentProviderUnion : public ContentProvider
{
public:
    ~ContentProviderUnion() override;

    void SetSlot(ContentProviderUnionSlot slot, ContentProvider * provider);
    void ClearSlot(ContentProviderUnionSlot slot);

    void Refresh() override;
    bool HasEntry(uint64_t title_id, LoaderContentRecordType type) const override;
    std::optional<u32> GetEntryVersion(uint64_t title_id) const override;
    VirtualFile GetEntryUnparsed(uint64_t title_id, LoaderContentRecordType type) const override;
    VirtualFile GetEntryRaw(uint64_t title_id, LoaderContentRecordType type) const override;
    std::unique_ptr<NCA> GetEntryNCA(uint64_t title_id, LoaderContentRecordType type) const override;
    std::vector<ContentProviderEntry> ListEntriesFilter(std::optional<LoaderTitleType> title_type, std::optional<LoaderContentRecordType> record_type, std::optional<uint64_t> title_id) const override;
    std::vector<std::pair<ContentProviderUnionSlot, ContentProviderEntry>> ListEntriesFilterOrigin(std::optional<ContentProviderUnionSlot> origin = {}, std::optional<LoaderTitleType> title_type = {}, std::optional<LoaderContentRecordType> record_type = {}, std::optional<uint64_t> title_id = {}) const;
    std::optional<ContentProviderUnionSlot> GetSlotForEntry(uint64_t title_id, LoaderContentRecordType type) const;
    const ContentProvider * GetSlotProvider(ContentProviderUnionSlot slot) const;
    const ManualContentProvider * GetManualContentProvider() const override;

private:
    std::map<ContentProviderUnionSlot, ContentProvider *> providers;
};

class ManualContentProvider : public ContentProvider
{
public:
    ~ManualContentProvider() override;

    void AddEntry(LoaderTitleType title_type, LoaderContentRecordType content_type, uint64_t title_id, uint32_t version, VirtualFile file);
    void ClearAllEntries();

    void Refresh() override;
    bool HasEntry(uint64_t title_id, LoaderContentRecordType type) const override;
    std::optional<u32> GetEntryVersion(uint64_t title_id) const override;
    VirtualFile GetEntryUnparsed(uint64_t title_id, LoaderContentRecordType type) const override;
    VirtualFile GetEntryRaw(uint64_t title_id, LoaderContentRecordType type) const override;
    std::unique_ptr<NCA> GetEntryNCA(uint64_t title_id, LoaderContentRecordType type) const override;
    std::vector<ContentProviderEntry> ListEntriesFilter(std::optional<LoaderTitleType> title_type, std::optional<LoaderContentRecordType> record_type, std::optional<uint64_t> title_id) const override;
    std::vector<ManualUpdateEntry> ListUpdateVersions(uint64_t title_id) const;
    VirtualFile GetEntryForVersion(uint64_t title_id, LoaderContentRecordType type, u32 version) const;

private:
    using ContentMap = std::map<std::tuple<LoaderTitleType, LoaderContentRecordType, uint64_t>, VirtualFile>;

    ContentMap entries;
    std::vector<ManualUpdateEntry> multi_version_entries;
};

} // namespace FileSys

class ManualContentProviderImpl :
    public IManualContentProvider
{
public:
    FileSys::ManualContentProvider & Provider();

    // IManualContentProvider
    void AddEntry(LoaderTitleType title_type, LoaderContentRecordType content_type, uint64_t title_id, uint32_t version, IVirtualFile * file) override;
    void ClearAllEntries() override;

private:
    FileSys::ManualContentProvider provider;
};
