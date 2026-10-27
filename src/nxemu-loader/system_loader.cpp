#include "system_loader.h"
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
#include "game_list_cache.h"
#include "loader_settings.h"
#include "loader_settings_identifiers.h"
#include "rom_info.h"
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>
#include <common/path.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

const char * const kSupportedGameExtensions[] = {
    "dxci",
    "dnsp",
    "nro",
};
constexpr uint32_t kSupportedGameExtensionCount = (uint32_t)(sizeof(kSupportedGameExtensions) / sizeof(kSupportedGameExtensions[0]));

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
        m_gameListCache(loader),
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
    GameListCache m_gameListCache;
    uint64_t m_processID;
    uint64_t m_titleID;

    // Filled by PrepareRom for StartPreparedRom.
    struct PreparedRom
    {
        std::string fileName;
        std::string title;
        std::vector<u8> nacpData;
        uint32_t version = 0;
        Loader::AppLoader::LoadParameters parameters{};
    };
    std::optional<PreparedRom> m_preparedRom;
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

void Systemloader::ResetSession()
{
    // Called after the guest OS and its borrowed filesystem controllers die.
    // Resetting only the loader's process ID leaves the old PID -> title/RomFS
    // entries alive and causes the next guest to read the previous game's data.
    impl->m_fsController.ResetSession();
    impl->m_appLoader.reset();
    impl->m_file.reset();
    impl->m_processID = 0;
    impl->m_titleID = 0;
    impl->m_preparedRom.reset();
    impl->m_manualContentProvider->ClearAllEntries();
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

bool Systemloader::LoadRom(const char * fileName, uint64_t program_id, int32_t program_index, int32_t previous_program_index, ApplicationLaunchType launch_type)
{
    return PrepareRom(fileName, program_id, program_index, previous_program_index, launch_type) && StartPreparedRom();
}

bool Systemloader::PrepareRom(const char * fileName, uint64_t program_id, int32_t program_index, int32_t previous_program_index, ApplicationLaunchType launch_type)
{
    impl->m_preparedRom.reset();
    g_settings->SetInt(NXCoreSetting::GameLoadResult, (int32_t)LoaderResultStatus::ErrorNotInitialized);
    if (fileName == nullptr || fileName[0] == '\0')
    {
        return false;
    }
    
    g_settings->SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::LoadingRom);
    impl->m_file = Core::GetGameFileFromPath(impl->m_virtualFilesystem, fileName);
    impl->m_appLoader = Loader::GetLoader(*this, impl->m_file, program_id, program_index);
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
    uint64_t loaded_program_id = 0;
    const LoaderResultStatus res = app_loader->ReadProgramId(loaded_program_id);
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
    impl->m_preparedRom = Impl::PreparedRom{fileName, std::move(title), std::move(nacp_data), pm.GetGameVersion().value_or(0), *load_parameters};
    return true;
}

bool Systemloader::StartPreparedRom()
{
    if (!impl->m_preparedRom)
    {
        return false;
    }
    Impl::PreparedRom prepared = std::move(*impl->m_preparedRom);
    impl->m_preparedRom.reset();
    IOperatingSystem & operatingSystem = impl->m_modules.OperatingSystem();

    g_settings->SetString(NXCoreSetting::GameName, prepared.title.c_str());
    g_settings->SetString(NXCoreSetting::GameFile, prepared.fileName.c_str());
    g_settings->SetBool(NXCoreSetting::EmulationRunning, true);
    // StartEmulation is invoked synchronously by this setting. A failed GPU
    // construction cancels the start before the application process is launched.
    if (!g_settings->GetBool(NXCoreSetting::EmulationRunning))
    {
        LOG_ERROR(Core, "Game launch aborted: guest GPU initialization failed");
        return false;
    }

    // TODO(DarkLordZach): When FSController/Game Card Support is added, if
    // current_process_game_card use correct StorageId
    StorageId baseGameStorageId = GetStorageIdForFrontendSlot(impl->m_contentProvider->GetSlotForEntry(impl->m_titleID, LoaderContentRecordType::Program));
    StorageId updateStorageId = GetStorageIdForFrontendSlot(impl->m_contentProvider->GetSlotForEntry(FileSys::GetUpdateTitleID(impl->m_titleID), LoaderContentRecordType::Program));

    if (!operatingSystem.StartApplicationProcess(prepared.parameters.main_thread_priority, prepared.parameters.main_thread_stack_size, prepared.version, baseGameStorageId, updateStorageId, prepared.nacpData.data(), (uint32_t)prepared.nacpData.size()))
    {
        g_settings->SetInt(NXCoreSetting::GameLoadResult, (int32_t)LoaderResultStatus::ErrorNotInitialized);
        g_settings->SetBool(NXCoreSetting::EmulationRunning, false);
        return false;
    }
    g_settings->SetInt(NXCoreSetting::GameLoadResult, (int32_t)LoaderResultStatus::Success);
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

bool Systemloader::ScanGameList(const char * const * gameDirs, const bool * scanSubfolders, uint32_t gameDirCount, IGameListScanCallback & callback)
{
    std::vector<GameDirectory> dirs;
    for (uint32_t i = 0; i < gameDirCount; i++)
    {
        if (gameDirs[i] != nullptr && gameDirs[i][0] != '\0')
        {
            dirs.push_back({gameDirs[i], scanSubfolders != nullptr && scanSubfolders[i]});
        }
    }
    return impl->m_gameListCache.Scan(dirs, callback);
}

bool Systemloader::ScanGameFiles(const GameFileEntry * files, uint32_t fileCount, IGameListScanCallback & callback)
{
    std::vector<GameListCache::ScanFile> list;
    list.reserve(fileCount);
    for (uint32_t i = 0; i < fileCount; i++)
    {
        if (files[i].path != nullptr && files[i].path[0] != '\0')
        {
            list.push_back({files[i].path, files[i].size, files[i].modifiedTime});
        }
    }
    return impl->m_gameListCache.ScanFiles(list, callback);
}

std::vector<GameDirectory> Systemloader::GameDirectories()
{
    return impl->m_gameListCache.Directories();
}

std::vector<std::string> Systemloader::GameDirectoryAddOnFiles(uint64_t baseProgramId)
{
    return impl->m_gameListCache.GameDirectoryAddOnFiles(baseProgramId);
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
