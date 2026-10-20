#include "game_list_cache.h"
#include "loader_settings.h"
#include "rom_info.h"
#include "system_loader.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/submission_package.h"
#include "core/loader/loader.h"
#include "nxemu-os/os_settings_identifiers.h"
#include <algorithm>
#include <common/path.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <yuzu_common/fs/fs.h>
#include <yuzu_common/fs/path_util.h>

extern IModuleSettings * g_settings;

namespace
{
constexpr uint32_t CacheMagic = 0x4C47584E; // "NXGL"
constexpr uint32_t CacheVersion = 3;
constexpr const char * CacheFileName = "game_list.bin";

std::string GetLowerExtension(const std::string & path)
{
    std::string ext = Path(path).GetExtension();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

bool IsGameExtension(const std::string & ext)
{
    return ext == "dxci" || ext == "dnsp" || ext == "nro";
}

bool IsAddOnExtension(const std::string & ext)
{
    return ext == "dxci" || ext == "dnsp" || ext == "nca";
}

bool IsUpdateDisabled(const std::vector<std::string> & disabled, uint32_t version)
{
    const std::string versioned = "Update@" + std::to_string(version);
    return std::find(disabled.begin(), disabled.end(), "Update") != disabled.end() ||
           std::find(disabled.begin(), disabled.end(), versioned) != disabled.end();
}

class Fingerprint
{
public:
    Fingerprint() :
        m_hash(0xCBF29CE484222325ULL)
    {
    }

    void Add(const void * data, size_t size)
    {
        const uint8_t * bytes = (const uint8_t *)data;
        for (size_t i = 0; i < size; i++)
        {
            m_hash ^= bytes[i];
            m_hash *= 0x100000001B3ULL;
        }
    }

    void Add(uint64_t value)
    {
        Add(&value, sizeof(value));
    }

    void Add(const std::string & value)
    {
        Add((uint64_t)value.size());
        Add(value.data(), value.size());
    }

    uint64_t Value() const
    {
        return m_hash;
    }

private:
    uint64_t m_hash;
};

class CacheWriter
{
public:
    void U32(uint32_t value)
    {
        Raw(&value, sizeof(value));
    }

    void U64(uint64_t value)
    {
        Raw(&value, sizeof(value));
    }

    void String(const std::string & value)
    {
        U32((uint32_t)value.size());
        Raw(value.data(), value.size());
    }

    void Bytes(const std::vector<uint8_t> & value)
    {
        U32((uint32_t)value.size());
        Raw(value.data(), value.size());
    }

    const std::vector<uint8_t> & Data() const
    {
        return m_data;
    }

private:
    void Raw(const void * data, size_t size)
    {
        const uint8_t * bytes = (const uint8_t *)data;
        m_data.insert(m_data.end(), bytes, bytes + size);
    }

    std::vector<uint8_t> m_data;
};

class CacheReader
{
public:
    explicit CacheReader(const std::vector<uint8_t> & data) :
        m_data(data),
        m_pos(0),
        m_ok(true)
    {
    }

    uint32_t U32()
    {
        uint32_t value = 0;
        Raw(&value, sizeof(value));
        return value;
    }

    uint64_t U64()
    {
        uint64_t value = 0;
        Raw(&value, sizeof(value));
        return value;
    }

    std::string String()
    {
        std::string value(Length(), '\0');
        Raw(value.data(), value.size());
        return value;
    }

    std::vector<uint8_t> Bytes()
    {
        std::vector<uint8_t> value(Length());
        Raw(value.data(), value.size());
        return value;
    }

    bool Ok() const
    {
        return m_ok;
    }

private:
    uint32_t Length()
    {
        const uint32_t length = U32();
        if (!m_ok || length > m_data.size() - m_pos)
        {
            m_ok = false;
            return 0;
        }
        return length;
    }

    void Raw(void * data, size_t size)
    {
        if (!m_ok || size > m_data.size() - m_pos)
        {
            m_ok = false;
            return;
        }
        if (size > 0)
        {
            std::memcpy(data, m_data.data() + m_pos, size);
        }
        m_pos += size;
    }

    const std::vector<uint8_t> & m_data;
    size_t m_pos;
    bool m_ok;
};

LoaderResultStatus CopyOut(const void * data, size_t size, void * buffer, uint32_t * bufferSize)
{
    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    if (buffer != nullptr && *bufferSize < (uint32_t)size)
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = (uint32_t)size;
    if (buffer != nullptr && size > 0)
    {
        std::memcpy(buffer, data, size);
    }
    return LoaderResultStatus::Success;
}

class CachedRomInfo :
    public IRomInfo
{
public:
    CachedRomInfo(Systemloader & systemloader, std::string path, std::shared_ptr<const CachedGameMetadata> metadata) :
        m_systemloader(systemloader),
        m_path(std::move(path)),
        m_metadata(std::move(metadata)),
        m_romInfo(nullptr)
    {
    }

    ~CachedRomInfo()
    {
        if (m_romInfo != nullptr)
        {
            m_romInfo->Release();
        }
    }

    // IRomInfo
    LoaderFileType GetFileType() const override
    {
        return m_metadata->fileType;
    }

    LoaderResultStatus ReadProgramId(uint64_t & out_program_id) override
    {
        if (m_metadata->programIdStatus == LoaderResultStatus::Success)
        {
            out_program_id = m_metadata->programId;
        }
        return m_metadata->programIdStatus;
    }

    LoaderResultStatus ReadTitle(char * buffer, uint32_t * bufferSize) override
    {
        if (m_metadata->titleStatus != LoaderResultStatus::Success)
        {
            return m_metadata->titleStatus;
        }
        return CopyOut(m_metadata->title.data(), m_metadata->title.size(), buffer, bufferSize);
    }

    LoaderResultStatus ReadDisplayVersion(char * buffer, uint32_t * bufferSize) override
    {
        if (m_metadata->versionStatus != LoaderResultStatus::Success)
        {
            return m_metadata->versionStatus;
        }
        return CopyOut(m_metadata->displayVersion.data(), m_metadata->displayVersion.size(), buffer, bufferSize);
    }

    LoaderResultStatus ReadIcon(uint8_t * buffer, uint32_t * bufferSize) override
    {
        if (m_metadata->iconStatus != LoaderResultStatus::Success)
        {
            return m_metadata->iconStatus;
        }
        return CopyOut(m_metadata->icon.data(), m_metadata->icon.size(), buffer, bufferSize);
    }

    LoaderResultStatus ReadProgramIds(uint64_t * buffer, uint32_t * count) override
    {
        if (m_metadata->programIdsStatus != LoaderResultStatus::Success)
        {
            return m_metadata->programIdsStatus;
        }
        if (count == nullptr)
        {
            return LoaderResultStatus::ErrorNotImplemented;
        }
        const uint32_t required = (uint32_t)m_metadata->programIds.size();
        if (buffer != nullptr && *count < required)
        {
            return LoaderResultStatus::ErrorBufferTooSmall;
        }
        *count = required;
        if (buffer != nullptr && required > 0)
        {
            std::memcpy(buffer, m_metadata->programIds.data(), required * sizeof(uint64_t));
        }
        return LoaderResultStatus::Success;
    }

    LoaderResultStatus ReadBanner(uint8_t * buffer, uint32_t * bufferSize) override
    {
        IRomInfo * romInfo = RealRomInfo();
        return romInfo != nullptr ? romInfo->ReadBanner(buffer, bufferSize) : LoaderResultStatus::ErrorNotInitialized;
    }

    LoaderResultStatus ReadLogo(uint8_t * buffer, uint32_t * bufferSize) override
    {
        IRomInfo * romInfo = RealRomInfo();
        return romInfo != nullptr ? romInfo->ReadLogo(buffer, bufferSize) : LoaderResultStatus::ErrorNotInitialized;
    }

    LoaderResultStatus Load(ISystemModules & modules, GuestProcessLoadParameters * out_parameters) override
    {
        IRomInfo * romInfo = RealRomInfo();
        return romInfo != nullptr ? romInfo->Load(modules, out_parameters) : LoaderResultStatus::ErrorNotInitialized;
    }

    void AddToManualContentProvider(IManualContentProvider & provider) override
    {
        IRomInfo * romInfo = RealRomInfo();
        if (romInfo != nullptr)
        {
            romInfo->AddToManualContentProvider(provider);
        }
    }

    void PrepareManualContent() override
    {
        IRomInfo * romInfo = RealRomInfo();
        if (romInfo != nullptr)
        {
            romInfo->PrepareManualContent();
        }
    }

    uint32_t GetGamePatches(GamePatchInfo * patches, uint32_t maxCount) override
    {
        IRomInfo * romInfo = RealRomInfo();
        return romInfo != nullptr ? romInfo->GetGamePatches(patches, maxCount) : 0;
    }

    IFileSysNACP * GetControlMetadata() override
    {
        IRomInfo * romInfo = RealRomInfo();
        return romInfo != nullptr ? romInfo->GetControlMetadata() : nullptr;
    }

    IVirtualFile * ReadManualRomFS() override
    {
        IRomInfo * romInfo = RealRomInfo();
        return romInfo != nullptr ? romInfo->ReadManualRomFS() : nullptr;
    }

    void Release() override
    {
        delete this;
    }

private:
    CachedRomInfo() = delete;
    CachedRomInfo(const CachedRomInfo &) = delete;
    CachedRomInfo & operator=(const CachedRomInfo &) = delete;

    IRomInfo * RealRomInfo()
    {
        std::lock_guard<std::mutex> lock(m_romInfoMutex);
        if (m_romInfo == nullptr)
        {
            const uint64_t programId = m_metadata->programIdStatus == LoaderResultStatus::Success ? m_metadata->programId : 0;
            m_romInfo = m_systemloader.RomInfo(m_path.c_str(), programId, 0);
        }
        return m_romInfo;
    }

    Systemloader & m_systemloader;
    std::string m_path;
    std::shared_ptr<const CachedGameMetadata> m_metadata;
    std::mutex m_romInfoMutex;
    IRomInfo * m_romInfo;
};

void WriteMetadata(CacheWriter & writer, const CachedGameMetadata & metadata)
{
    writer.U32((uint32_t)metadata.fileType);
    writer.U32((uint32_t)metadata.programIdStatus);
    writer.U64(metadata.programId);
    writer.U32((uint32_t)metadata.programIdsStatus);
    writer.U32((uint32_t)metadata.programIds.size());
    for (uint64_t programId : metadata.programIds)
    {
        writer.U64(programId);
    }
    writer.U32((uint32_t)metadata.titleStatus);
    writer.String(metadata.title);
    writer.U32((uint32_t)metadata.iconStatus);
    writer.Bytes(metadata.icon);
    writer.U32((uint32_t)metadata.versionStatus);
    writer.String(metadata.displayVersion);
}

void ReadMetadata(CacheReader & reader, CachedGameMetadata & metadata)
{
    metadata.fileType = (LoaderFileType)reader.U32();
    metadata.programIdStatus = (LoaderResultStatus)reader.U32();
    metadata.programId = reader.U64();
    metadata.programIdsStatus = (LoaderResultStatus)reader.U32();
    const uint32_t programIdCount = reader.U32();
    for (uint32_t i = 0; i < programIdCount && reader.Ok(); i++)
    {
        metadata.programIds.push_back(reader.U64());
    }
    metadata.titleStatus = (LoaderResultStatus)reader.U32();
    metadata.title = reader.String();
    metadata.iconStatus = (LoaderResultStatus)reader.U32();
    metadata.icon = reader.Bytes();
    metadata.versionStatus = (LoaderResultStatus)reader.U32();
    metadata.displayVersion = reader.String();
}

void ReadLoaderMetadata(Loader::AppLoader & loader, CachedGameMetadata & metadata)
{
    metadata.fileType = loader.GetFileType();
    metadata.programIdStatus = loader.ReadProgramId(metadata.programId);

    uint32_t count = 0;
    metadata.programIdsStatus = loader.ReadProgramIds(nullptr, &count);
    if (metadata.programIdsStatus == LoaderResultStatus::Success)
    {
        metadata.programIds.resize(count);
        metadata.programIdsStatus = loader.ReadProgramIds(metadata.programIds.data(), &count);
    }

    uint32_t size = 0;
    metadata.titleStatus = loader.ReadTitle(nullptr, &size);
    if (metadata.titleStatus == LoaderResultStatus::Success)
    {
        metadata.title.resize(size);
        metadata.titleStatus = loader.ReadTitle(metadata.title.data(), &size);
    }

    size = 0;
    metadata.iconStatus = loader.ReadIcon(nullptr, &size);
    if (metadata.iconStatus == LoaderResultStatus::Success)
    {
        metadata.icon.resize(size);
        metadata.iconStatus = loader.ReadIcon(metadata.icon.data(), &size);
    }

    FileSys::NACP nacp;
    metadata.versionStatus = loader.ReadControlData(nacp);
    if (metadata.versionStatus == LoaderResultStatus::Success)
    {
        metadata.displayVersion = nacp.GetVersionString();
    }
}

std::filesystem::path CacheFilePath()
{
    return Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir) / CacheFileName;
}

} // namespace

GameListCache::GameListCache(Systemloader & systemloader) :
    m_systemloader(systemloader),
    m_loaded(false)
{
}

GameListCache::~GameListCache()
{
}

bool GameListCache::Scan(const std::vector<GameDirectory> & gameDirs, IGameListScanCallback & callback)
{
    std::lock_guard<std::mutex> lock(m_scanMutex);
    if (!m_loaded)
    {
        Load();
        m_loaded = true;
    }
    m_gameDirectories = gameDirs;

    m_disabledAddOns = loaderSettings.disabled_addons;
    for (GameFiles::value_type & game : m_gameFiles)
    {
        game.second.seen = false;
    }

    bool complete = ScanAddOnDirectories(callback);
    for (size_t i = 0; complete && i < gameDirs.size(); i++)
    {
        complete = ScanGameDirectory(gameDirs[i], callback);
    }

    if (complete)
    {
        UpdateIndex addOnIndex;
        for (const AddOnFiles::value_type & addOnFile : m_addOnFiles)
        {
            for (const AddOnEntry & entry : addOnFile.second.entries)
            {
                if (entry.titleType == LoaderTitleType::Update && entry.contentType == LoaderContentRecordType::Control)
                {
                    addOnIndex.emplace(entry.titleId, UpdateCandidate{&addOnFile.first, addOnFile.second.stamp, entry.version});
                }
            }
        }

        for (GameFiles::iterator itr = m_gameFiles.begin(); itr != m_gameFiles.end();)
        {
            itr = itr->second.seen ? std::next(itr) : m_gameFiles.erase(itr);
        }

        UpdateIndex gameDirIndex;
        for (const GameFiles::value_type & game : m_gameFiles)
        {
            for (const AddOnEntry & entry : game.second.addOns)
            {
                if (entry.titleType == LoaderTitleType::Update && entry.contentType == LoaderContentRecordType::Control)
                {
                    gameDirIndex.emplace(entry.titleId, UpdateCandidate{&game.first, game.second.stamp, entry.version});
                }
            }
        }
        for (GameFiles::value_type & game : m_gameFiles)
        {
            if (!game.second.isGame)
            {
                continue;
            }
            for (ProgramRecord & program : game.second.programs)
            {
                if (RefreshUpdate(program, addOnIndex, gameDirIndex))
                {
                    ReportProgram(game.first, program, true, callback);
                }
            }
        }
    }
    Save();
    return complete;
}

std::vector<GameDirectory> GameListCache::Directories()
{
    std::lock_guard<std::mutex> lock(m_scanMutex);
    if (!m_loaded)
    {
        Load();
        m_loaded = true;
    }
    return m_gameDirectories;
}

std::vector<std::string> GameListCache::GameDirectoryAddOnFiles(uint64_t baseProgramId)
{
    std::lock_guard<std::mutex> lock(m_scanMutex);
    if (!m_loaded)
    {
        Load();
        m_loaded = true;
    }
    std::vector<std::string> files;
    for (const GameFiles::value_type & game : m_gameFiles)
    {
        const bool matches = std::any_of(game.second.addOns.begin(), game.second.addOns.end(), [baseProgramId](const AddOnEntry & entry) {
            return FileSys::GetBaseTitleID(entry.titleId) == baseProgramId;
        });
        if (matches)
        {
            files.push_back(game.first);
        }
    }
    return files;
}

bool GameListCache::ScanAddOnDirectories(IGameListScanCallback & callback)
{
    for (AddOnFiles::value_type & addOnFile : m_addOnFiles)
    {
        addOnFile.second.seen = false;
    }

    bool stopped = false;
    for (const std::string & dir : loaderSettings.addOnDirectories)
    {
        if (dir.empty())
        {
            continue;
        }
        const auto callbackEntry = [&](const std::filesystem::directory_entry & entry) -> bool {
            if (callback.ScanStopRequested())
            {
                stopped = true;
                return false;
            }
            const std::string path = Common::FS::PathToUTF8String(entry.path());
            if (!IsAddOnExtension(GetLowerExtension(path)))
            {
                return true;
            }
            std::error_code ec;
            FileStamp stamp;
            stamp.size = entry.file_size(ec);
            stamp.modifiedTime = (int64_t)entry.last_write_time(ec).time_since_epoch().count();

            const std::pair<AddOnFiles::iterator, bool> added = m_addOnFiles.try_emplace(path);
            AddOnFileRecord & record = added.first->second;
            if (added.second || !(record.stamp == stamp))
            {
                record.entries = ReadAddOnEntries(path);
                record.stamp = stamp;
            }
            record.seen = true;
            return true;
        };
        Common::FS::IterateDirEntriesRecursively(dir, callbackEntry, Common::FS::DirEntryFilter::File);
        if (stopped)
        {
            return false;
        }
    }

    for (AddOnFiles::iterator itr = m_addOnFiles.begin(); itr != m_addOnFiles.end();)
    {
        itr = itr->second.seen ? std::next(itr) : m_addOnFiles.erase(itr);
    }
    return true;
}

bool GameListCache::ScanGameDirectory(const GameDirectory & gameDirectory, IGameListScanCallback & callback)
{
    const std::string & gameDir = gameDirectory.path;
    struct DirFile
    {
        std::string path;
        FileStamp stamp;
        bool game;
    };
    std::vector<DirFile> files;

    bool stopped = false;
    const auto callbackEntry = [&](const std::filesystem::directory_entry & entry) -> bool {
        if (callback.ScanStopRequested())
        {
            stopped = true;
            return false;
        }
        std::string path = Common::FS::PathToUTF8String(entry.path());
        const std::string ext = GetLowerExtension(path);
        const bool game = IsGameExtension(ext), addOn = IsAddOnExtension(ext);
        if (!game && !addOn)
        {
            return true;
        }
        std::error_code ec;
        FileStamp stamp;
        stamp.size = entry.file_size(ec);
        stamp.modifiedTime = (int64_t)entry.last_write_time(ec).time_since_epoch().count();
        files.push_back({std::move(path), stamp, game});
        return true;
    };
    if (gameDirectory.scanSubfolders)
    {
        Common::FS::IterateDirEntriesRecursively(gameDir, callbackEntry, Common::FS::DirEntryFilter::File);
    }
    else
    {
        Common::FS::IterateDirEntries(gameDir, callbackEntry, Common::FS::DirEntryFilter::File);
    }
    if (stopped)
    {
        return false;
    }

    std::vector<bool> parse(files.size(), false);
    for (size_t i = 0; i < files.size(); i++)
    {
        GameFiles::iterator itr = m_gameFiles.find(files[i].path);
        parse[i] = itr == m_gameFiles.end() || !(itr->second.stamp == files[i].stamp);
    }

    const uint32_t parseTotal = static_cast<uint32_t>(std::count(parse.begin(), parse.end(), true));
    uint32_t parseStarted = 0;
    for (size_t i = 0; i < files.size(); i++)
    {
        if (callback.ScanStopRequested())
        {
            callback.ScanProgress(0, 0);
            return false;
        }
        GameFiles::value_type & game = *m_gameFiles.try_emplace(files[i].path).first;
        GameFileRecord & record = game.second;
        record.seen = true;

        if (parse[i])
        {
            callback.ScanProgress(++parseStarted, parseTotal);
            record = GameFileRecord();
            record.seen = true;
            record.stamp = files[i].stamp;
            ParseGameFile(game.first, record);
        }
        if (record.isGame)
        {
            ReportGame(game.first, record, parse[i], callback);
        }
    }

    if (parseTotal != 0)
    {
        callback.ScanProgress(0, 0);
    }
    return true;
}

void GameListCache::ParseGameFile(const std::string & path, GameFileRecord & record) const
{
    const std::string ext = GetLowerExtension(path);
    if (IsGameExtension(ext))
    {
        FileSys::VirtualFile file = m_systemloader.GetFilesystem()->OpenFile(path, VirtualFileOpenMode::Read);
        std::shared_ptr<Loader::AppLoader> loader = file ? Loader::GetLoader(m_systemloader, file, 0, 0) : nullptr;
        const LoaderFileType fileType = loader ? loader->GetFileType() : LoaderFileType::Error;
        if (fileType != LoaderFileType::Unknown && fileType != LoaderFileType::Error)
        {
            record.isGame = true;
            record.programs.emplace_back();
            ReadLoaderMetadata(*loader, record.programs.back().base);

            // Some cartridges and packages bundle several applications (e.g. Super Mario Galaxy + Super Mario
            // Galaxy 2). List each application separately, as the Switch home menu does.
            const std::vector<uint64_t> programIds = record.programs.back().base.programIds;
            if (programIds.size() > 1 && (fileType == LoaderFileType::XCI || fileType == LoaderFileType::NSP))
            {
                record.programs.clear();
                for (uint64_t programId : programIds)
                {
                    std::shared_ptr<Loader::AppLoader> programLoader = Loader::GetLoader(m_systemloader, file, programId, 0);
                    if (!programLoader || programLoader->GetFileType() != fileType)
                    {
                        continue;
                    }
                    ProgramRecord program;
                    ReadLoaderMetadata(*programLoader, program.base);
                    if (program.base.programIdStatus == LoaderResultStatus::Success && program.base.programId == programId)
                    {
                        record.programs.push_back(std::move(program));
                    }
                }
                if (record.programs.empty())
                {
                    record.programs.emplace_back();
                    ReadLoaderMetadata(*loader, record.programs.back().base);
                }
            }
        }
    }
    if (IsAddOnExtension(ext))
    {
        record.addOns = ReadAddOnEntries(path);
    }
}

GameListCache::AddOnEntries GameListCache::ReadAddOnEntries(const std::string & path) const
{
    AddOnEntries entries;
    FileSys::VirtualFile file = m_systemloader.GetFilesystem()->OpenFile(path, VirtualFileOpenMode::Read);
    if (!file)
    {
        return entries;
    }

    const auto addNsp = [&entries](const FileSys::NSP & nsp) {
        if (nsp.GetStatus() != LoaderResultStatus::Success)
        {
            return;
        }
        UpdateVersionMap versions;
        CollectUpdateVersions(nsp, versions);
        for (const auto & title : nsp.GetNCAs())
        {
            for (const auto & content : title.second)
            {
                const LoaderTitleType titleType = content.first.first;
                if (titleType != LoaderTitleType::Update && titleType != LoaderTitleType::AOC)
                {
                    continue;
                }
                AddOnEntry entry;
                entry.titleId = title.first;
                entry.titleType = titleType;
                entry.contentType = content.first.second;
                if (titleType == LoaderTitleType::Update)
                {
                    UpdateVersionMap::const_iterator itr = versions.find(title.first);
                    entry.version = itr != versions.end() ? itr->second : 0;
                }
                entries.push_back(entry);
            }
        }
    };

    const std::string ext = GetLowerExtension(path);
    if (ext == "dnsp")
    {
        addNsp(FileSys::NSP(file));
    }
    else if (ext == "dxci")
    {
        const FileSys::XCI xci(file);
        if (xci.GetStatus() == LoaderResultStatus::Success)
        {
            const std::shared_ptr<FileSys::NSP> nsp = xci.GetSecurePartitionNSP();
            if (nsp)
            {
                addNsp(*nsp);
            }
        }
    }
    else if (ext == "nca")
    {
        const FileSys::NCA nca(file);
        if (nca.GetStatus() != LoaderResultStatus::Success && nca.GetStatus() != LoaderResultStatus::ErrorMissingBKTRBaseRomFS)
        {
            return entries;
        }
        const uint64_t titleId = nca.GetTitleId();
        if (FileSys::GetBaseTitleID(titleId) == titleId)
        {
            return entries;
        }
        AddOnEntry entry;
        entry.titleId = titleId;
        entry.titleType = (titleId & 0x800) != 0 ? LoaderTitleType::Update : LoaderTitleType::AOC;
        entry.contentType = FileSys::GetCRTypeFromNCAType(nca.GetType());
        entries.push_back(entry);
    }
    return entries;
}

bool GameListCache::RefreshUpdate(ProgramRecord & program, const UpdateIndex & addOnIndex, const UpdateIndex & gameDirIndex) const
{
    const uint64_t programId = program.base.programIdStatus == LoaderResultStatus::Success ? program.base.programId : 0;
    const uint64_t updateTitleId = FileSys::GetUpdateTitleID(programId);
    const DisabledAddOns::const_iterator disabledItr = m_disabledAddOns.find(programId);
    static const std::vector<std::string> noneDisabled;
    const std::vector<std::string> & disabled = disabledItr != m_disabledAddOns.end() ? disabledItr->second : noneDisabled;

    const UpdateCandidate * best = nullptr;
    if (programId != 0)
    {
        for (const UpdateIndex * index : {&addOnIndex, &gameDirIndex})
        {
            const auto range = index->equal_range(updateTitleId);
            for (UpdateIndex::const_iterator itr = range.first; itr != range.second; ++itr)
            {
                if (IsUpdateDisabled(disabled, itr->second.version))
                {
                    continue;
                }
                if (best == nullptr || itr->second.version > best->version)
                {
                    best = &itr->second;
                }
            }
        }
    }

    Fingerprint fingerprint;
    if (best != nullptr)
    {
        fingerprint.Add(*best->path);
        fingerprint.Add(best->stamp.size);
        fingerprint.Add((uint64_t)best->stamp.modifiedTime);
        fingerprint.Add(best->version);
    }
    if (program.updateValid && program.updateFingerprint == fingerprint.Value())
    {
        return false;
    }

    program.updateTitle.clear();
    program.updateIcon.clear();
    program.updateVersion.clear();
    if (best != nullptr)
    {
        ReadUpdateMetadata(*best, updateTitleId, program);
    }
    program.updateValid = true;
    program.updateFingerprint = fingerprint.Value();
    program.effective.reset();
    return true;
}

void GameListCache::ReadUpdateMetadata(const UpdateCandidate & candidate, uint64_t updateTitleId, ProgramRecord & program) const
{
    FileSys::VirtualFile file = m_systemloader.GetFilesystem()->OpenFile(*candidate.path, VirtualFileOpenMode::Read);
    if (!file)
    {
        return;
    }

    std::shared_ptr<FileSys::NCA> control;
    const std::string ext = GetLowerExtension(*candidate.path);
    if (ext == "dnsp")
    {
        control = FileSys::NSP(file).GetNCA(updateTitleId, LoaderContentRecordType::Control, LoaderTitleType::Update);
    }
    else if (ext == "dxci")
    {
        const FileSys::XCI xci(file);
        const std::shared_ptr<FileSys::NSP> nsp = xci.GetStatus() == LoaderResultStatus::Success ? xci.GetSecurePartitionNSP() : nullptr;
        if (nsp)
        {
            control = nsp->GetNCA(updateTitleId, LoaderContentRecordType::Control, LoaderTitleType::Update);
        }
    }
    else if (ext == "nca")
    {
        control = std::make_shared<FileSys::NCA>(file);
    }
    if (!control || control->GetStatus() != LoaderResultStatus::Success)
    {
        return;
    }

    const FileSys::PatchManager::Metadata metadata = FileSys::ParseControlRomFS(control->RomFS());
    if (metadata.first)
    {
        program.updateTitle = metadata.first->GetApplicationName();
        program.updateVersion = metadata.first->GetVersionString();
    }
    if (metadata.second)
    {
        program.updateIcon = metadata.second->ReadAllBytes();
    }
}

void GameListCache::ReportGame(const std::string & path, GameFileRecord & record, bool changed, IGameListScanCallback & callback)
{
    for (ProgramRecord & program : record.programs)
    {
        ReportProgram(path, program, changed, callback);
    }
}

void GameListCache::ReportProgram(const std::string & path, ProgramRecord & program, bool changed, IGameListScanCallback & callback)
{
    if (!program.effective)
    {
        std::shared_ptr<CachedGameMetadata> effective = std::make_shared<CachedGameMetadata>(program.base);
        if (!program.updateTitle.empty())
        {
            effective->title = program.updateTitle;
            effective->titleStatus = LoaderResultStatus::Success;
        }
        if (!program.updateIcon.empty())
        {
            effective->icon = program.updateIcon;
            effective->iconStatus = LoaderResultStatus::Success;
        }
        if (!program.updateVersion.empty())
        {
            effective->displayVersion = program.updateVersion;
            effective->versionStatus = LoaderResultStatus::Success;
        }
        program.effective = std::move(effective);
    }
    callback.GameFound(path.c_str(), new CachedRomInfo(m_systemloader, path, program.effective), changed);
}

void GameListCache::Load()
{
    std::ifstream stream(CacheFilePath(), std::ios::binary);
    if (!stream)
    {
        return;
    }
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());

    CacheReader reader(data);
    if (reader.U32() != CacheMagic || reader.U32() != CacheVersion)
    {
        return;
    }
    if (reader.U32() != (uint32_t)g_settings->GetInt(NXOsSetting::LanguageIndex))
    {
        return;
    }

    AddOnFiles addOnFiles;
    GameFiles gameFiles;

    const auto readAddOns = [&reader](AddOnEntries & entries) {
        const uint32_t count = reader.U32();
        for (uint32_t i = 0; i < count && reader.Ok(); i++)
        {
            AddOnEntry entry;
            entry.titleId = reader.U64();
            entry.titleType = (LoaderTitleType)reader.U32();
            entry.contentType = (LoaderContentRecordType)reader.U32();
            entry.version = reader.U32();
            entries.push_back(entry);
        }
    };

    const uint32_t addOnCount = reader.U32();
    for (uint32_t i = 0; i < addOnCount && reader.Ok(); i++)
    {
        std::string path = reader.String();
        AddOnFileRecord & record = addOnFiles[path];
        record.stamp.size = reader.U64();
        record.stamp.modifiedTime = (int64_t)reader.U64();
        readAddOns(record.entries);
    }

    const uint32_t gameCount = reader.U32();
    for (uint32_t i = 0; i < gameCount && reader.Ok(); i++)
    {
        std::string path = reader.String();
        GameFileRecord & record = gameFiles[path];
        record.stamp.size = reader.U64();
        record.stamp.modifiedTime = (int64_t)reader.U64();
        record.isGame = reader.U32() != 0;
        const uint32_t programCount = reader.U32();
        for (uint32_t j = 0; j < programCount && reader.Ok(); j++)
        {
            ProgramRecord & program = record.programs.emplace_back();
            ReadMetadata(reader, program.base);
            program.updateValid = reader.U32() != 0;
            program.updateFingerprint = reader.U64();
            program.updateTitle = reader.String();
            program.updateIcon = reader.Bytes();
            program.updateVersion = reader.String();
        }
        readAddOns(record.addOns);
    }

    std::vector<GameDirectory> gameDirectories;
    const uint32_t gameDirectoryCount = reader.U32();
    for (uint32_t i = 0; i < gameDirectoryCount && reader.Ok(); i++)
    {
        GameDirectory gameDirectory;
        gameDirectory.path = reader.String();
        gameDirectory.scanSubfolders = reader.U32() != 0;
        gameDirectories.push_back(std::move(gameDirectory));
    }

    if (!reader.Ok())
    {
        return;
    }
    m_addOnFiles = std::move(addOnFiles);
    m_gameFiles = std::move(gameFiles);
    m_gameDirectories = std::move(gameDirectories);
}

void GameListCache::Save() const
{
    CacheWriter writer;
    writer.U32(CacheMagic);
    writer.U32(CacheVersion);
    writer.U32((uint32_t)g_settings->GetInt(NXOsSetting::LanguageIndex));

    const auto writeAddOns = [&writer](const AddOnEntries & entries) {
        writer.U32((uint32_t)entries.size());
        for (const AddOnEntry & entry : entries)
        {
            writer.U64(entry.titleId);
            writer.U32((uint32_t)entry.titleType);
            writer.U32((uint32_t)entry.contentType);
            writer.U32(entry.version);
        }
    };

    writer.U32((uint32_t)m_addOnFiles.size());
    for (const AddOnFiles::value_type & addOnFile : m_addOnFiles)
    {
        writer.String(addOnFile.first);
        writer.U64(addOnFile.second.stamp.size);
        writer.U64((uint64_t)addOnFile.second.stamp.modifiedTime);
        writeAddOns(addOnFile.second.entries);
    }

    writer.U32((uint32_t)m_gameFiles.size());
    for (const GameFiles::value_type & game : m_gameFiles)
    {
        const GameFileRecord & record = game.second;
        writer.String(game.first);
        writer.U64(record.stamp.size);
        writer.U64((uint64_t)record.stamp.modifiedTime);
        writer.U32(record.isGame ? 1 : 0);
        writer.U32((uint32_t)record.programs.size());
        for (const ProgramRecord & program : record.programs)
        {
            WriteMetadata(writer, program.base);
            writer.U32(program.updateValid ? 1 : 0);
            writer.U64(program.updateFingerprint);
            writer.String(program.updateTitle);
            writer.Bytes(program.updateIcon);
            writer.String(program.updateVersion);
        }
        writeAddOns(record.addOns);
    }

    writer.U32((uint32_t)m_gameDirectories.size());
    for (const GameDirectory & gameDirectory : m_gameDirectories)
    {
        writer.String(gameDirectory.path);
        writer.U32(gameDirectory.scanSubfolders ? 1u : 0u);
    }

    const std::filesystem::path cacheFile = CacheFilePath();
    std::filesystem::path tempFile = cacheFile;
    tempFile += ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(cacheFile.parent_path(), ec);
    {
        std::ofstream stream(tempFile, std::ios::binary | std::ios::trunc);
        if (!stream)
        {
            return;
        }
        const std::vector<uint8_t> & data = writer.Data();
        stream.write((const char *)data.data(), (std::streamsize)data.size());
        if (!stream)
        {
            return;
        }
    }
    std::filesystem::rename(tempFile, cacheFile, ec);
}
