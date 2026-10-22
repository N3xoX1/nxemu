#include "system_loader.h"
#include <nxemu-module-spec/game_content_cache_store.h>
#include "core/file_sys/vfs/backing_read_traffic.h"
#include <nxemu-module-spec/application_control_data.h>
#include "core/core.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/filesystem.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/system_archive/system_archive.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_ivirtualfile.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/service/set/settings_types.h"
#include "core/loader/loader.h"
#include "firmware_install.h"
#include "loader_settings.h"
#include "loader_settings_identifiers.h"
#include "rom_info.h"
#include <algorithm>
#include <cctype>
#include <common/path.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <yuzu_common/fs/path_util.h>
#include <charconv>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <system_error>
#include <fmt/core.h>
#include <nxemu-core/settings/identifiers.h>
#include <yuzu_common/fs/fs.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/fs_util.h>

extern IModuleSettings * g_settings;
extern IModuleNotification * g_notify;

namespace
{
StorageId GetStorageIdForFrontendSlot(std::optional<FileSys::ContentProviderUnionSlot> slot)
{
    if (!slot.has_value())
    {
        return StorageId::None;
    }

    switch (*slot)
    {
    case FileSys::ContentProviderUnionSlot::UserNAND: return StorageId::NandUser;
    case FileSys::ContentProviderUnionSlot::SysNAND: return StorageId::NandSystem;
    case FileSys::ContentProviderUnionSlot::SDMC: return StorageId::SdCard;
    case FileSys::ContentProviderUnionSlot::FrontendManual: return StorageId::Host;
    }
    return StorageId::None;
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

const char * const kSupportedGameExtensions[] = {
    "dxci",
    "dnsp",
    "nro",
};
constexpr uint32_t kSupportedGameExtensionCount = (uint32_t)(sizeof(kSupportedGameExtensions) / sizeof(kSupportedGameExtensions[0]));

// The independent content cache is a hint only. Each entry is used only
// after its signature has been matched against the actual file in the scan.
using LibraryDiskEntry = GameContentCacheStore::Entry;

std::filesystem::path ContentCacheIndexPath()
{
    // Modules have their own path manager. Use the frontend's configured data
    // directory so both sides share the same persistent content index.
    return GameContentCacheStore::IndexPath(std::filesystem::path{
        Common::FS::ToU8String(g_settings->GetString(NXCoreSetting::AppDirectory))});
}

std::unordered_map<std::string, LibraryDiskEntry> LoadContentCache()
{
    GameContentCacheStore::Entries contents;
    GameContentCacheStore::Read(ContentCacheIndexPath(), contents);
    return contents;
}


bool HasSupportedFileExtension(const char * fileName)
{
    std::string ext = Path(fileName).GetExtension();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (uint32_t i = 0; i < kSupportedGameExtensionCount; ++i)
    {
        if (ext == kSupportedGameExtensions[i])
        {
            return true;
        }
    }
    return false;
}

} // Anonymous namespace

class HostFilesystemImpl final :
    public IFilesystem
{
public:
    explicit HostFilesystemImpl(FileSys::VirtualFilesystem & vfs) :
        m_vfs(vfs)
    {
    }

    IVirtualDirectory * CreateDirectory(const char * path, VirtualFileOpenMode perms) override
    {
        if (!m_vfs || path == nullptr)
        {
            return nullptr;
        }
        FileSys::VirtualDir dir = m_vfs->CreateDirectory(path, perms);
        if (!dir)
        {
            return nullptr;
        }
        return std::make_unique<VirtualDirectoryImpl>(dir).release();
    }

private:
    FileSys::VirtualFilesystem & m_vfs;
};

struct Systemloader::Impl
{
    explicit Impl(Systemloader & loader, ISystemModules & modules) :
        m_loader(loader),
        m_modules(modules),
        m_fsController(loader),
        m_manualContentProvider(std::make_unique<ManualContentProviderImpl>()),
        m_hostFilesystem(m_virtualFilesystem),
        m_processID(0),
        m_titleID(0)
    {
    }
    ~Impl()
    {
        m_appLoader.reset();
    }

    FileSys::VirtualFile m_file;
    std::shared_ptr<Loader::AppLoader> m_appLoader;
    Systemloader & m_loader;
    ISystemModules & m_modules;
    /// RealVfsFilesystem instance
    FileSys::VirtualFilesystem m_virtualFilesystem;
    std::unique_ptr<FileSys::ContentProviderUnion> m_contentProvider;
    ::FileSystemController m_fsController;
    std::unique_ptr<ManualContentProviderImpl> m_manualContentProvider;
    HostFilesystemImpl m_hostFilesystem;
    uint64_t m_processID;
    uint64_t m_titleID;

    using IndexedFile = GameContentCacheStore::Entry;
    struct ContentIndex
    {
        std::unordered_map<std::string, IndexedFile> files;
        std::unordered_map<uint64_t, std::vector<std::string>> filesByApplication;
        std::unordered_map<uint64_t, std::vector<std::string>> modificationRoots;
    };
    // Build serialization never blocks readers: they only acquire the small
    // publication mutex long enough to take an immutable snapshot pointer.
    std::mutex m_contentIndexBuildMutex;
    std::mutex m_contentIndexMutex;
    std::shared_ptr<const ContentIndex> m_contentIndex;
    bool m_diskCacheRead = false; // guarded by m_contentIndexBuildMutex
};

Systemloader::Systemloader(ISystemModules & modules) :
    impl(std::make_unique<Impl>(*this, modules))
{
}

Systemloader::~Systemloader()
{
    impl->m_appLoader.reset();
    impl->m_file.reset();
    impl.reset();
}

bool Systemloader::Initialize()
{
    SetupLoaderSetting();
    if (impl->m_virtualFilesystem == nullptr)
    {
        impl->m_virtualFilesystem = std::make_shared<FileSys::RealVfsFilesystem>();
    }
    if (impl->m_contentProvider == nullptr)
    {
        impl->m_contentProvider = std::make_unique<FileSys::ContentProviderUnion>();
    }
    impl->m_contentProvider->SetSlot(FileSys::ContentProviderUnionSlot::FrontendManual, &impl->m_manualContentProvider->Provider());
    FirmwareInstall::RecoverInterrupted();
    impl->m_fsController.CreateFactories(*impl->m_virtualFilesystem, false);
    return true;
}

uint32_t Systemloader::GetSupportedGameExtensions(const char ** extensions, uint32_t maxCount) const
{
    if (extensions != nullptr && maxCount > 0)
    {
        const uint32_t copyCount = maxCount < kSupportedGameExtensionCount ? maxCount : kSupportedGameExtensionCount;
        for (uint32_t i = 0; i < copyCount; ++i)
        {
            extensions[i] = kSupportedGameExtensions[i];
        }
    }
    return kSupportedGameExtensionCount;
}

bool Systemloader::LoadRom(const char * fileName, uint64_t selected_program_id, int32_t program_index, int32_t previous_program_index, ApplicationLaunchType launch_type)
{
    g_settings->SetInt(NXCoreSetting::GameLoadResult, (int32_t)LoaderResultStatus::ErrorNotInitialized);
    if (fileName == nullptr || fileName[0] == '\0')
    {
        return false;
    }
    
    RefreshGameContentIndex();
    g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::LoadingRom);
    impl->m_file = Core::GetGameFileFromPath(impl->m_virtualFilesystem, fileName);
    impl->m_appLoader = Loader::GetLoader(*this, impl->m_file, selected_program_id, program_index);
    if (!impl->m_appLoader)
    {
        g_notify->DisplayError("The file format is not supported.", "Error loading file!");
        g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopped);
        return false;
    }

    g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::RomLoaded);
    Loader::AppLoader * app_loader = impl->m_appLoader.get();
    const LoaderFileType file_type = impl->m_appLoader->GetFileType();
    if (file_type == LoaderFileType::Unknown || file_type == LoaderFileType::Error)
    {
        g_notify->DisplayError("The file format is not supported.", "Error loading file!");
        g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopped);
        return false;
    }
    uint64_t program_id = 0;
    const LoaderResultStatus res = app_loader->ReadProgramId(program_id);
    if (res == LoaderResultStatus::Success)
    {
        ManualContentProvider().ClearAllEntries();
        {
            ::RomInfo loadedRom(*this, impl->m_file, impl->m_appLoader);
            loadedRom.PrepareManualContent();
        }
    }
    else
    {
        LOG_ERROR(Core, "Failed to find title id for ROM!");
    }

    uint32_t title_size = 0;
    std::string title = "Unknown program";
    LoaderResultStatus result = app_loader->ReadTitle(nullptr, &title_size);
    if (result == LoaderResultStatus::Success)
    {
        title.resize(title_size);
        result = app_loader->ReadTitle(title.data(), &title_size);
    }
    if (result != LoaderResultStatus::Success)
    {
        LOG_ERROR(Core, "Failed to read title for ROM!");
    }

    IOperatingSystem & operatingSystem = impl->m_modules.OperatingSystem();
    operatingSystem.SetApplicationLaunchParameters(program_index, previous_program_index, launch_type);

    const auto [load_result, load_parameters] = app_loader->Load(*this, impl->m_modules);
    if (load_result != LoaderResultStatus::Success)
    {
        g_settings->SetInt(NXCoreSetting::GameLoadResult, (int32_t)load_result);
        LOG_CRITICAL(Core, "Failed to load ROM (Error {})!", load_result);
        g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopped);
        return false;
    }
    std::vector<u8> nacp_data;
    FileSys::NACP nacp;
    if (app_loader->ReadControlData(nacp) == LoaderResultStatus::Success)
    {
        nacp_data = nacp.GetRawBytes();
    }
    else
    {
        nacp_data.resize(sizeof(FileSys::RawNACP));
    }

    FileSys::PatchManager pm(impl->m_titleID, impl->m_fsController, *impl->m_contentProvider);
    uint32_t version = pm.GetGameVersion().value_or(0);

    g_settings->SetString(NXCoreSetting::GameName, title.c_str());
    g_settings->SetString(NXCoreSetting::GameFile, fileName);
    g_settings->SetBool(NXCoreSetting::EmulationRunning, true);

    // TODO(DarkLordZach): When FSController/Game Card Support is added, if
    // current_process_game_card use correct StorageId
    StorageId baseGameStorageId = GetStorageIdForFrontendSlot(impl->m_contentProvider->GetSlotForEntry(impl->m_titleID, LoaderContentRecordType::Program));
    StorageId updateStorageId = GetStorageIdForFrontendSlot(impl->m_contentProvider->GetSlotForEntry(FileSys::GetUpdateTitleID(impl->m_titleID), LoaderContentRecordType::Program));

    g_settings->SetInt(NXCoreSetting::GameLoadResult, (int32_t)LoaderResultStatus::Success);
    operatingSystem.StartApplicationProcess(load_parameters->main_thread_priority, load_parameters->main_thread_stack_size, version, baseGameStorageId, updateStorageId, nacp_data.data(), (uint32_t)nacp_data.size());
    return true;
}

IRomInfo * Systemloader::RomInfo(const char * fileName, uint64_t programId, uint64_t programIndex)
{
    if (!HasSupportedFileExtension(fileName))
    {
        return nullptr;
    }
    const FileSys::VirtualFile file = impl->m_virtualFilesystem->OpenFile(fileName, VirtualFileOpenMode::Read);
    if (!file)
    {
        return nullptr;
    }

    std::shared_ptr<Loader::AppLoader> app_loader = Loader::GetLoader(*this, file, programId, programIndex);
    if (!app_loader)
    {
        return nullptr;
    }
    return std::make_unique<::RomInfo>(*this, file, std::move(app_loader)).release();
}

IRomInfo * Systemloader::FileRomInfo(IVirtualFile * file, uint64_t programId, uint64_t programIndex)
{
    if (file == nullptr)
    {
        return nullptr;
    }

    IVirtualFile * const owned_file = file->Duplicate();
    if (owned_file == nullptr)
    {
        return nullptr;
    }

    const FileSys::VirtualFile vfile = std::make_shared<VfsVirtualFile>(owned_file);
    std::shared_ptr<Loader::AppLoader> app_loader = Loader::GetLoader(*this, vfile, programId, programIndex);
    if (!app_loader)
    {
        return nullptr;
    }

    return std::make_unique<::RomInfo>(*this, vfile, std::move(app_loader)).release();
}

IRomInfo * Systemloader::LoadedRomInfo()
{
    std::unique_ptr<::RomInfo> info = std::make_unique<::RomInfo>(*this, impl->m_file, impl->m_appLoader);
    return info.release();
}

FileSys::VirtualFilesystem Systemloader::GetFilesystem()
{
    return impl->m_virtualFilesystem;
}

FileSystemController & Systemloader::GetFileSystemController()
{
    return impl->m_fsController;
}

FileSys::ContentProvider & Systemloader::GetContentProvider()
{
    return *impl->m_contentProvider;
}

void Systemloader::RegisterContentProvider(FileSys::ContentProviderUnionSlot slot, FileSys::ContentProvider * provider)
{
    impl->m_contentProvider->SetSlot(slot, provider);
}

void Systemloader::SetProcessID(uint64_t processID)
{
    impl->m_processID = processID;
}

void Systemloader::SetTitleID(uint64_t titleID)
{
    impl->m_titleID = titleID;
}

IFileSystemController & Systemloader::FileSystemController()
{
    return impl->m_fsController;
}

IContentProvider & Systemloader::ContentProvider()
{
    return *impl->m_contentProvider;
}

IFilesystem & Systemloader::Filesystem()
{
    return impl->m_hostFilesystem;
}

IVirtualFile * Systemloader::SynthesizeSystemArchive(const uint64_t title_id)
{
    FileSys::VirtualFile file = FileSys::SystemArchive::SynthesizeSystemArchive(title_id);
    if (file)
    {
        return std::make_unique<VirtualFileImpl>(file).release();
    }
    return nullptr;
}

IVirtualFile * Systemloader::CreateMemoryFile(const uint8_t * data, uint64_t size, const char * name)
{
    if (name == nullptr)
    {
        return nullptr;
    }
    if (size > 0 && data == nullptr)
    {
        return nullptr;
    }

    std::vector<u8> copy;
    if (size > 0)
    {
        copy.assign(data, data + size);
    }

    FileSys::VirtualFile file = std::make_shared<FileSys::VectorVfsFile>(std::move(copy), name);
    return std::make_unique<VirtualFileImpl>(file).release();
}

uint32_t Systemloader::GetContentProviderEntriesCount(bool useTitleType, LoaderTitleType titleType, bool useContentRecordType, LoaderContentRecordType contentRecordType, bool useTitleId, unsigned long long titleId)
{
    const FileSys::ContentProviderUnion & rcu = *impl->m_contentProvider;
    std::optional<LoaderTitleType> title = useTitleType ? std::optional<LoaderTitleType>((LoaderTitleType)titleType) : std::nullopt;
    std::optional<LoaderContentRecordType> record = useContentRecordType ? std::optional<LoaderContentRecordType>((LoaderContentRecordType)contentRecordType) : std::nullopt;
    std::optional<uint64_t> id = useTitleId ? std::optional<uint64_t>(titleId) : std::nullopt;
    const std::vector<ContentProviderEntry> list = rcu.ListEntriesFilter(title, record, id);
    return (uint32_t)list.size();
}

uint32_t Systemloader::GetContentProviderEntries(bool useTitleType, LoaderTitleType titleType, bool useContentRecordType, LoaderContentRecordType contentRecordType, bool useTitleId, unsigned long long titleId, ContentProviderEntry * entries, uint32_t entryCount)
{
    if (entries == nullptr || entryCount == 0)
    {
        return 0;
    }
    const FileSys::ContentProviderUnion & rcu = *impl->m_contentProvider;
    std::optional<LoaderTitleType> title = useTitleType ? std::optional<LoaderTitleType>((LoaderTitleType)titleType) : std::nullopt;
    std::optional<LoaderContentRecordType> record = useContentRecordType ? std::optional<LoaderContentRecordType>((LoaderContentRecordType)contentRecordType) : std::nullopt;
    std::optional<uint64_t> id = useTitleId ? std::optional<uint64_t>(titleId) : std::nullopt;
    const std::vector<ContentProviderEntry> list = rcu.ListEntriesFilter(title, record, id);
    entryCount = std::min((uint32_t)list.size(), entryCount);
    memcpy(entries, list.data(), entryCount * sizeof(ContentProviderEntry));
    return entryCount;
}

IFileSysNCA * Systemloader::GetContentProviderEntry(uint64_t title_id, LoaderContentRecordType type)
{
    return GetContentProvider().GetEntryNCA(title_id, type).release();
}

IFileSysNACP * Systemloader::ReadControlData()
{
    if (impl->m_appLoader == nullptr)
    {
        return nullptr;
    }

    FileSys::NACP nacp;
    if (impl->m_appLoader->ReadControlData(nacp) != LoaderResultStatus::Success)
    {
        return nullptr;
    }

    return new FileSys::NACP(nacp);
}

IFileSysNACP * Systemloader::GetPMControlMetadata(uint64_t programID)
{
    const FileSys::PatchManager pm(programID, GetFileSystemController(), GetContentProvider());
    FileSys::PatchManager::Metadata metadata = pm.GetControlMetadata();
    if (metadata.first == nullptr)
    {
        return nullptr;
    }
    return metadata.first.release();
}

LoaderResultStatus Systemloader::GetPMControlData(uint64_t program_id, uint8_t * buffer,
                                                 uint32_t buffer_size, uint32_t & actual_size)
{
    actual_size = 0;
    const FileSys::PatchManager pm(program_id, GetFileSystemController(), GetContentProvider());
    const auto metadata = pm.GetControlMetadata();
    if (metadata.first == nullptr || !metadata.first->IsValid())
    {
        return LoaderResultStatus::ErrorNoControl;
    }
    static_assert(sizeof(FileSys::RawNACP) == ApplicationControlData::NacpSize);
    const auto nacp = metadata.first->GetRawBytes();
    const auto & icon = metadata.second;
    return ApplicationControlData::Read(nacp, icon != nullptr ? icon->GetSize() : 0,
        [&icon](uint8_t * destination, uint64_t size) {
            return icon->Read(destination, static_cast<size_t>(size), 0);
        }, buffer, buffer_size, actual_size);
}

void Systemloader::RefreshGameContentIndex()
{
    // Only concurrent rebuilds wait for filesystem I/O. Readers keep using
    // the last complete snapshot while the next one is being assembled.
    std::lock_guard buildLock(impl->m_contentIndexBuildMutex);
    const std::vector<std::string> configured = ConfiguredGameDirectories();
    std::shared_ptr<const Impl::ContentIndex> previous;
    {
        std::lock_guard lock(impl->m_contentIndexMutex);
        previous = impl->m_contentIndex;
    }
    std::unordered_map<std::string, LibraryDiskEntry> persisted;
    if (!impl->m_diskCacheRead)
        persisted = LoadContentCache();

    auto next = std::make_shared<Impl::ContentIndex>();
    std::unordered_set<std::string> seen;
    const auto index_mod = [&next](const std::filesystem::path & path) {
        const std::string name = Common::FS::PathToUTF8String(path.filename());
        if (name.size() != 16) return;
        uint64_t id = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), id, 16);
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size())
            next->modificationRoots[id].push_back(Common::FS::PathToUTF8String(path));
    };
    std::unordered_set<std::string> seenRoots;
    for (const std::string & root : configured)
    {
        if (!seenRoots.insert(root).second) continue;
        std::error_code error;
        const std::filesystem::path dir{Common::FS::ToU8String(root)};
        if (!std::filesystem::is_directory(dir, error) || error)
        {
            LOG_WARNING(Core, "Game content index not refreshed: directory {} is unavailable ({})", root,
                        error ? error.message() : "not a directory");
            return;
        }
        index_mod(dir);
        std::filesystem::recursive_directory_iterator it(dir, error), end;
        if (error)
        {
            LOG_WARNING(Core, "Game content index not refreshed: cannot open {} ({})", root, error.message());
            return;
        }
        while (it != end)
        {
            std::error_code file_error;
            const auto & entry = *it;
            if (entry.is_directory(file_error) && !file_error)
                index_mod(entry.path());
            if (file_error)
            {
                LOG_WARNING(Core, "Game content index not refreshed: cannot inspect {} ({})",
                            Common::FS::PathToUTF8String(entry.path()), file_error.message());
                return;
            }
            if (entry.is_regular_file(file_error) && !file_error)
            {
                const auto extension = ToLowerAscii(Common::FS::PathToUTF8String(entry.path().extension()));
                if (extension == ".dnsp" || extension == ".dxci" || extension == ".nca")
                {
                    const std::string path = Common::FS::PathToUTF8String(entry.path());
                    if (seen.insert(path).second)
                    {
                        const uintmax_t size = entry.file_size(file_error);
                        const auto modified = !file_error ? entry.last_write_time(file_error) : std::filesystem::file_time_type{};
                        if (file_error)
                        {
                            LOG_WARNING(Core, "Game content index not refreshed: cannot stat {} ({})", path, file_error.message());
                            return;
                        }
                        Impl::IndexedFile file;
                        file.signature = std::to_string(size) + ":" +
                            std::to_string(static_cast<long long>(modified.time_since_epoch().count()));
                        const auto prev = previous ? previous->files.find(path) : next->files.end();
                        if (previous && prev != previous->files.end() && prev->second.signature == file.signature &&
                            !prev->second.applications.empty())
                            file.applications = prev->second.applications;
                        else if (const auto saved = persisted.find(path);
                                 saved != persisted.end() && saved->second.signature == file.signature && !saved->second.applications.empty())
                            file.applications = saved->second.applications;
                        else
                        {
                            uint64_t ids[16]{};
                            const uint32_t count = GetContainerApplications(path.c_str(), ids, 16);
                            if (count <= 16) file.applications.assign(ids, ids + count);
                            else
                            {
                                file.applications.resize(count);
                                const uint32_t read = GetContainerApplications(path.c_str(), file.applications.data(), count);
                                file.applications.resize(std::min(read, count));
                            }
                        }
                        for (uint64_t app : file.applications)
                            next->filesByApplication[app].push_back(path);
                        next->files.emplace(path, std::move(file));
                    }
                }
            }
            if (file_error)
            {
                LOG_WARNING(Core, "Game content index not refreshed: cannot inspect {} ({})",
                            Common::FS::PathToUTF8String(entry.path()), file_error.message());
                return;
            }
            it.increment(error);
            if (error)
            {
                LOG_WARNING(Core, "Game content index not refreshed: cannot traverse {} ({})", root, error.message());
                return;
            }
        }
    }
    for (auto & [id, files] : next->filesByApplication)
        std::sort(files.begin(), files.end());
    for (auto & [id, roots] : next->modificationRoots)
    {
        std::sort(roots.begin(), roots.end());
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    }
    // Standalone launches also keep the shared metadata warm across restarts.
    if (configured != ConfiguredGameDirectories()) return;
    // Unknown containers are retried rather than persisted as negative results.
    GameContentCacheStore::Entries durable;
    for (const auto & [path, entry] : next->files)
        if (!entry.applications.empty()) durable.emplace(path, entry);
    const bool changed = impl->m_diskCacheRead ? !previous || previous->files != next->files : persisted != durable;
    const bool saved = !changed || GameContentCacheStore::Write(ContentCacheIndexPath(), durable);
    if (!saved)
        LOG_WARNING(Core, "Could not save game content metadata");
    {
        std::lock_guard lock(impl->m_contentIndexMutex);
        impl->m_contentIndex = std::move(next);
    }
    impl->m_diskCacheRead = saved;
}

bool Systemloader::PublishGameContentSnapshot(const GameContentSnapshot & snapshot)
{
    std::lock_guard buildLock(impl->m_contentIndexBuildMutex);
    // A scan of a former configuration is not an authoritative new index.
    const auto configured = ConfiguredGameDirectories();
    const auto normalized = [](std::vector<std::string> roots) {
        std::sort(roots.begin(), roots.end());
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
        return roots;
    };
    if (normalized(configured) != normalized(snapshot.directories))
        return false;

    auto next = std::make_shared<Impl::ContentIndex>();
    for (const auto & entry : snapshot.files)
    {
        // Duplicate paths are expected when the same root was configured
        // twice; each file must contribute exactly once.
        if (!next->files.emplace(entry.path, Impl::IndexedFile{entry.signature, entry.applications}).second)
            continue;
        for (uint64_t application : entry.applications)
            next->filesByApplication[application].push_back(entry.path);
    }
    for (const std::string & path : snapshot.modificationRoots)
    {
        const std::string name = Common::FS::PathToUTF8String(
            std::filesystem::path{Common::FS::ToU8String(path)}.filename());
        if (name.size() != 16) continue;
        uint64_t id = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), id, 16);
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size())
            next->modificationRoots[id].push_back(path);
    }
    for (auto & [id, paths] : next->filesByApplication)
    {
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    }
    for (auto & [id, paths] : next->modificationRoots)
    {
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    }
    if (configured != ConfiguredGameDirectories()) return false;
    {
        std::lock_guard lock(impl->m_contentIndexMutex);
        impl->m_contentIndex = std::move(next);
    }
    // The next full loader rebuild must still be able to use the disk cache.
    return true;
}

std::vector<std::string> Systemloader::GameContentFiles(uint64_t base_program_id)
{
    std::shared_ptr<const Impl::ContentIndex> index;
    {
        std::lock_guard lock(impl->m_contentIndexMutex);
        index = impl->m_contentIndex;
    }
    if (!index)
    {
        RefreshGameContentIndex();
        std::lock_guard lock(impl->m_contentIndexMutex);
        index = impl->m_contentIndex;
    }
    if (!index) return {};
    const auto it = index->filesByApplication.find(base_program_id);
    return it == index->filesByApplication.end() ? std::vector<std::string>{} : it->second;
}

std::vector<std::string> Systemloader::GameModificationRoots(uint64_t program_id)
{
    std::shared_ptr<const Impl::ContentIndex> index;
    {
        std::lock_guard lock(impl->m_contentIndexMutex);
        index = impl->m_contentIndex;
    }
    if (!index)
    {
        RefreshGameContentIndex();
        std::lock_guard lock(impl->m_contentIndexMutex);
        index = impl->m_contentIndex;
    }
    if (!index) return {};
    const auto it = index->modificationRoots.find(program_id);
    return it == index->modificationRoots.end() ? std::vector<std::string>{} : it->second;
}

bool Systemloader::IsInGameDirectory(const std::string & file_path) const
{
    const std::filesystem::path file{Common::FS::ToU8String(file_path)};
    for (const auto & directory : ConfiguredGameDirectories())
    {
        const auto relative = file.lexically_relative(std::filesystem::path{Common::FS::ToU8String(directory)});
        if (!relative.empty() && *relative.begin() != "..") return true;
    }
    return false;
}

uint32_t Systemloader::GetContainerApplications(const char * utf8_path, uint64_t * application_ids, uint32_t max_count)
{
    const FileSys::VirtualFile file = utf8_path != nullptr ? impl->m_virtualFilesystem->OpenFile(utf8_path, VirtualFileOpenMode::Read) : nullptr;
    if (!file)
    {
        return 0;
    }

    std::vector<uint64_t> applications;
    const auto add = [&applications](uint64_t title_id) {
        const uint64_t application = FileSys::GetBaseTitleID(title_id);
        if (std::find(applications.begin(), applications.end(), application) == applications.end())
        {
            applications.push_back(application);
        }
    };
    const auto addPackage = [&add](const FileSys::NSP & nsp) {
        if (nsp.GetStatus() != LoaderResultStatus::Success)
        {
            return;
        }
        for (const auto & title : nsp.GetNCAs())
        {
            add(title.first);
        }
    };

    const std::string extension = ToLowerAscii(file->GetExtension());
    if (extension == "dnsp")
    {
        addPackage(FileSys::NSP(file));
    }
    else if (extension == "dxci")
    {
        const FileSys::XCI xci(file);
        const std::shared_ptr<FileSys::NSP> nsp = xci.GetStatus() == LoaderResultStatus::Success ? xci.GetSecurePartitionNSP() : nullptr;
        if (nsp)
        {
            addPackage(*nsp);
        }
    }
    else if (extension == "nca")
    {
        const FileSys::NCA nca(file);
        if (nca.GetStatus() == LoaderResultStatus::Success || nca.GetStatus() == LoaderResultStatus::ErrorMissingBKTRBaseRomFS)
        {
            add(nca.GetTitleId());
        }
    }

    if (application_ids != nullptr)
    {
        std::copy_n(applications.begin(), std::min<size_t>(applications.size(), max_count), application_ids);
    }
    return static_cast<uint32_t>(applications.size());
}

IManualContentProvider & Systemloader::ManualContentProvider()
{
    return *(impl->m_manualContentProvider.get());
}

void Systemloader::SetDisabledAddons(uint64_t program_id, const char * const * names, uint32_t count)
{
    std::vector<std::string> disabled;
    if (names != nullptr)
    {
        disabled.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            if (names[i] != nullptr && names[i][0] != '\0')
            {
                disabled.emplace_back(names[i]);
            }
        }
    }
    SetLoaderDisabledAddons(program_id, std::move(disabled));
}

bool Systemloader::IsAddonDisabled(uint64_t program_id, const char * name) const
{
    if (name == nullptr || name[0] == '\0')
    {
        return false;
    }

    const TitleIdStringListMap::const_iterator itr = loaderSettings.disabled_addons.find(program_id);
    if (itr == loaderSettings.disabled_addons.end())
    {
        return false;
    }
    return std::find(itr->second.begin(), itr->second.end(), name) != itr->second.end();
}

uint32_t Systemloader::GetInstalledFirmwareDisplayVersion(char * buffer, uint32_t buffer_size) const
{
    if (buffer == nullptr || buffer_size == 0)
    {
        return 0;
    }
    buffer[0] = '\0';
    FirmwareInstall::Version firmware{};
    if (!FirmwareInstall::ReadInstalledVersion(impl->m_fsController, firmware))
    {
        return 0;
    }
    const std::string version = FirmwareInstall::DisplayVersion(firmware);
    const uint32_t length = static_cast<uint32_t>(std::min(version.size(), static_cast<size_t>(buffer_size - 1)));
    std::memcpy(buffer, version.data(), length);
    buffer[length] = '\0';
    return length;
}

// A game card carries the firmware its game needs.
uint32_t Systemloader::GetGameCardFirmwareUpdate(const char * utf8_path, char * versionBuffer, uint32_t versionBufferSize) const
{
    if (utf8_path == nullptr || utf8_path[0] == '\0' || versionBuffer == nullptr || versionBufferSize == 0)
    {
        return 0;
    }
    versionBuffer[0] = '\0';
    FirmwareInstall::Version package{};
    if (!FirmwareInstall::ReadGameCardVersion(impl->m_virtualFilesystem, std::filesystem::path{Common::FS::ToU8String(utf8_path)}, package))
    {
        return 0;
    }
    FirmwareInstall::Version installed{};
    if (FirmwareInstall::ReadInstalledVersion(impl->m_fsController, installed) && !FirmwareInstall::IsNewer(package, installed))
    {
        return 0;
    }
    const std::string version = FirmwareInstall::DisplayVersion(package);
    const uint32_t length = static_cast<uint32_t>(std::min(version.size(), static_cast<size_t>(versionBufferSize - 1)));
    std::memcpy(versionBuffer, version.data(), length);
    versionBuffer[length] = '\0';
    return length;
}

FirmwareInstallStatus Systemloader::InstallFirmwarePackage(const char * utf8_path, IFirmwareInstallObserver * observer, char * versionBuffer,
                                                           uint32_t versionBufferSize)
{
    if (versionBuffer != nullptr && versionBufferSize != 0)
    {
        versionBuffer[0] = '\0';
    }
    if (utf8_path == nullptr || utf8_path[0] == '\0')
    {
        return FirmwareInstallStatus::SourceUnreadable;
    }
    const FirmwareInstall::Result result =
        FirmwareInstall::Install(std::filesystem::path{Common::FS::ToU8String(utf8_path)}, impl->m_virtualFilesystem, impl->m_fsController, observer);
    if (versionBuffer != nullptr && versionBufferSize != 0)
    {
        const size_t length = std::min(result.version.size(), static_cast<size_t>(versionBufferSize - 1));
        std::memcpy(versionBuffer, result.version.data(), length);
        versionBuffer[length] = '\0';
    }
    return result.status;
}

void Systemloader::GetBackingReadTraffic(StorageReadTraffic* out) const {
    *out = FileSys::backing_read_traffic;
}
