#pragma once
#include "base.h"
#include "operating_system.h"
#include <string>
#include <vector>

enum class LoaderTitleType : uint8_t {
    SystemProgram = 0x01,
    SystemDataArchive = 0x02,
    SystemUpdate = 0x03,
    FirmwarePackageA = 0x04,
    FirmwarePackageB = 0x05,
    Application = 0x80,
    Update = 0x81,
    AOC = 0x82,
    DeltaTitle = 0x83,
};

enum class LoaderContentRecordType : uint8_t {
    Meta = 0,
    Program = 1,
    Data = 2,
    Control = 3,
    HtmlDocument = 4,
    LegalInformation = 5,
    DeltaFragment = 6,
};

enum class LoaderResultStatus : uint16_t {
    Success,
    ErrorAlreadyLoaded,
    ErrorNotImplemented,
    ErrorNotInitialized,
    ErrorBadNPDMHeader,
    ErrorBadACIDHeader,
    ErrorBadACIHeader,
    ErrorBadFileAccessControl,
    ErrorBadFileAccessHeader,
    ErrorBadKernelCapabilityDescriptors,
    ErrorBadPFSHeader,
    ErrorIncorrectPFSFileSize,
    ErrorBadNCAHeader,
    ErrorMissingProductionKeyFile,
    ErrorMissingHeaderKey,
    ErrorIncorrectHeaderKey,
    ErrorNCA2,
    ErrorNCA0,
    ErrorMissingTitlekey,
    ErrorMissingTitlekek,
    ErrorInvalidRightsID,
    ErrorMissingKeyAreaKey,
    ErrorIncorrectKeyAreaKey,
    ErrorIncorrectTitlekeyOrTitlekek,
    ErrorXCIMissingProgramNCA,
    ErrorNCANotProgram,
    ErrorNoExeFS,
    ErrorBadXCIHeader,
    ErrorXCIMissingPartition,
    ErrorNullFile,
    ErrorMissingNPDM,
    Error32BitISA,
    ErrorUnableToParseKernelMetadata,
    ErrorNoRomFS,
    ErrorIncorrectELFFileSize,
    ErrorLoadingNRO,
    ErrorLoadingNSO,
    ErrorNoIcon,
    ErrorNoControl,
    ErrorBadNAXHeader,
    ErrorIncorrectNAXFileSize,
    ErrorNAXKeyHMACFailed,
    ErrorNAXValidationHMACFailed,
    ErrorNAXKeyDerivationFailed,
    ErrorNAXInconvertibleToNCA,
    ErrorBadNAXFilePath,
    ErrorMissingSDSeed,
    ErrorMissingSDKEKSource,
    ErrorMissingAESKEKGenerationSource,
    ErrorMissingAESKeyGenerationSource,
    ErrorMissingSDSaveKeySource,
    ErrorMissingSDNCAKeySource,
    ErrorNSPMissingProgramNCA,
    ErrorBadBKTRHeader,
    ErrorBKTRSubsectionNotAfterRelocation,
    ErrorBKTRSubsectionNotAtEnd,
    ErrorBadRelocationBlock,
    ErrorBadSubsectionBlock,
    ErrorBadRelocationBuckets,
    ErrorBadSubsectionBuckets,
    ErrorMissingBKTRBaseRomFS,
    ErrorNoPackedUpdate,
    ErrorBadKIPHeader,
    ErrorBLZDecompressionFailed,
    ErrorBadINIHeader,
    ErrorINITooManyKIPs,
    ErrorIntegrityVerificationNotImplemented,
    ErrorIntegrityVerificationFailed,
    ErrorBufferTooSmall,
};

enum class VirtualFileOpenMode : uint32_t {
    Read = (1 << 0),
    Write = (1 << 1),
    AllowAppend = (1 << 2),

    ReadWrite = (Read | Write),
    All = (ReadWrite | AllowAppend),
};

enum class SaveDataSpaceId : uint8_t
{
    System = 0,
    User = 1,
    SdSystem = 2,
    Temporary = 3,
    SdUser = 4,

    ProperSystem = 100,
    SafeMode = 101,
};

enum class SaveDataType : uint8_t
{
    System = 0,
    Account = 1,
    Bcat = 2,
    Device = 3,
    Temporary = 4,
    Cache = 5,
    SystemBcat = 6,
};

enum class SaveDataRank : uint8_t
{
    Primary = 0,
    Secondary = 1,
};

enum class LoaderFileType {
    Error,
    Unknown,
    NSO,
    NRO,
    NCA,
    NSP,
    XCI,
    NAX,
    KIP,
    DeconstructedRomDirectory,
};

struct ContentProviderEntry
{
    uint64_t titleID;
    LoaderContentRecordType type;
};

enum class GamePatchType : uint8_t
{
    Update,
    AddOnContent,
    Mod,
};

struct GamePatchInfo
{
    bool enabled;
    GamePatchType type;
    char name[128];
    char version[128];
    char key[128];
};

// One piece of a game's add-on content.
struct GameAddOnContentInfo
{
    uint64_t titleId;
    uint32_t version;
    uint64_t size; // of its data, in bytes
};

struct SaveDataAttribute 
{
    uint64_t program_id;
    uint64_t user_id[2];
    uint64_t system_save_data_id;
    SaveDataType type;
    SaveDataRank rank;
    uint16_t index;
    uint8_t padding[0x1C];
};
static_assert(sizeof(SaveDataAttribute) == 0x40);

struct SaveDataSize {
    uint64_t normal;
    uint64_t journal;
};
static_assert(sizeof(SaveDataSize) == 0x10, "SaveDataSize has invalid size.");

struct VirtualFileTimeStampRaw {
    uint64_t created{};
    uint64_t modified{};
    uint64_t accessed{};
    uint8_t is_valid{};
    uint8_t padding[7]{};
};
static_assert(sizeof(VirtualFileTimeStampRaw) == 0x20);

nxinterface IVirtualFile;
nxinterface IVirtualDirectoryList;
nxinterface IVirtualFileList;

nxinterface IVirtualDirectory
{
    virtual IVirtualDirectoryList * GetSubdirectories() const = 0;
    virtual IVirtualDirectory * CreateSubdirectory(const char * path) const = 0;
    virtual IVirtualDirectory * GetDirectoryRelative(const char * path) const = 0;
    virtual IVirtualDirectory * GetSubdirectory(const char * name) const = 0;
    virtual IVirtualDirectory * Duplicate() = 0;
    virtual IVirtualFileList * GetFiles() const = 0;
    virtual IVirtualFile * CreateFile(const char * name) const = 0;
    virtual IVirtualFile * GetFile(const char * name) const = 0;
    virtual IVirtualFile * GetFileRelative(const char * relative_path) const = 0;
    virtual IVirtualFile * OpenFile(const char * path, VirtualFileOpenMode perms) = 0;
    virtual bool DeleteFile(const char * name) const = 0;
    virtual bool DeleteSubdirectory(const char * name) const = 0;
    virtual bool DeleteSubdirectoryRecursive(const char * name) const = 0;
    virtual bool CleanSubdirectoryRecursive(const char * name) const = 0;
    virtual bool RenameDirectory(const char * old_path, const char * new_path) const = 0;
    virtual bool GetFileTimeStamp(VirtualFileTimeStampRaw * out, const char * path) const = 0;
    virtual const char * GetName() const = 0;
    virtual void Release() = 0;
};

nxinterface IVirtualDirectoryList
{
    virtual uint32_t GetSize() const = 0;
    virtual IVirtualDirectory * GetItem(uint32_t index) const = 0;
    virtual void Release() = 0;
};

nxinterface IVirtualFile
{
    virtual uint64_t GetSize() const = 0;
    virtual const char * GetName() const = 0;
    virtual const char * GetFullPath() const = 0;
    virtual bool Resize(uint64_t size) = 0;
    virtual uint64_t ReadBytes(uint8_t * data, uint64_t datalen, uint64_t offset) = 0;
    virtual uint64_t WriteBytes(const uint8_t * data, uint64_t datalen, uint64_t offset) = 0;
    virtual IVirtualDirectory * ExtractRomFS() = 0;
    virtual IVirtualDirectory * GetContainingDirectory() const = 0;
    virtual IVirtualFile * Duplicate() = 0;
    virtual bool Rename(const char * name) = 0;
    virtual void Release() = 0;
};

nxinterface IVirtualFileList
{
    virtual uint32_t GetSize() const = 0;
    virtual IVirtualFile * GetItem(uint32_t index) const = 0;
    virtual void Release() = 0;
};

nxinterface IFilesystem
{
    virtual IVirtualDirectory * CreateDirectory(const char * path, VirtualFileOpenMode perms) = 0;
};

nxinterface ISaveDataFactory
{
    virtual bool CreateSaveData(IVirtualDirectory ** out_save_data, SaveDataSpaceId space, const SaveDataAttribute & attribute) = 0;
    virtual bool OpenSaveData(IVirtualDirectory ** out_save_data, SaveDataSpaceId space, const SaveDataAttribute & attribute) = 0;
    virtual void SetAutoCreate(bool state) = 0;
    virtual void Release() = 0;
};

nxinterface IRomFsController
{
    virtual IVirtualFile * OpenRomFS(uint64_t title_id, StorageId storage_id, LoaderContentRecordType type) = 0;
    virtual IVirtualFile * OpenRomFSCurrentProcess() = 0;
    virtual IVirtualFile * PatchBaseNca(uint64_t title_id, StorageId storage_id, LoaderContentRecordType type, IVirtualFile & romfs) = 0;
    virtual void Release() = 0;
};

nxinterface IFileSysNACP
{
    virtual uint32_t GetSupportedLanguages() const = 0;
    virtual uint32_t GetParentalControlFlag() const = 0;
    virtual uint16_t GetCacheStorageMaxIndex() const = 0;
    virtual uint64_t GetCacheStorageDataAndJournalMaxSize() const = 0;
    virtual bool GetUserAccountSwitchLock() const = 0;
    virtual bool GetRatingAge(uint8_t * buffer, uint32_t bufferSize) const = 0;
    virtual const char * GetVersionString() const = 0;
    virtual const char * GetDeveloperName() const = 0;
    virtual void Release() = 0;
};

nxinterface IFileSysNCA
{
    virtual LoaderResultStatus GetStatus() const = 0;
    virtual IVirtualFile * GetBaseFile() = 0;
    virtual IVirtualFile * GetRomFS() = 0;
    virtual void Release() = 0;
};

nxinterface IFileSysRegisteredCache
{
    virtual IFileSysNCA * GetEntry(uint64_t title_id, LoaderContentRecordType type) const = 0;
};

nxinterface ISaveDataController
{
    virtual bool CreateSaveData(IVirtualDirectory** out_save_data, SaveDataSpaceId space, const SaveDataAttribute & attribute) = 0;
    virtual SaveDataSize ReadSaveDataSize(SaveDataType type, uint64_t title_id, uint64_t user_id_high, uint64_t user_id_low) = 0;
    virtual void WriteSaveDataSize(SaveDataType type, uint64_t title_id, uint64_t user_id_high, uint64_t user_id_low, SaveDataSize new_value) = 0;
    virtual void Release() = 0;
};

nxinterface IFileSystemController
{
    virtual IFileSysRegisteredCache & GetSystemNANDContents() const = 0;
    virtual ISaveDataController * OpenSaveDataController() const = 0;
    virtual bool OpenProcess(uint64_t * programId, ISaveDataFactory ** saveDataFactory, IRomFsController ** romFsController, uint64_t processId) = 0;
    virtual uint64_t GetFreeSpaceSize(StorageId id) const = 0;
    virtual uint64_t GetTotalSpaceSize(StorageId id) const = 0;

    virtual bool OpenSDMC(IVirtualDirectory** out_sdmc) const = 0;
    virtual IVirtualDirectory * OpenBCATDirectory(uint64_t title_id) const = 0;
};

nxinterface IContentProvider
{
    virtual IFileSysNCA * GetEntry(uint64_t title_id, LoaderContentRecordType type) const = 0;
};

nxinterface IManualContentProvider
{
    virtual void AddEntry(LoaderTitleType title_type, LoaderContentRecordType content_type, uint64_t title_id, uint32_t version, IVirtualFile * file) = 0;
    virtual void ClearAllEntries() = 0;
};

struct GuestProcessLoadParameters
{
    int32_t main_thread_priority;
    int64_t main_thread_stack_size;
};

nxinterface IRomInfo
{
    virtual LoaderFileType GetFileType() const = 0;
    virtual LoaderResultStatus ReadProgramId(uint64_t & out_program_id) = 0;
    virtual LoaderResultStatus ReadTitle(char * buffer, uint32_t * bufferSize) = 0;
    virtual LoaderResultStatus ReadDisplayVersion(char * buffer, uint32_t * bufferSize) = 0;
    virtual LoaderResultStatus ReadIcon(uint8_t * buffer, uint32_t * bufferSize) = 0;
    virtual LoaderResultStatus ReadBanner(uint8_t * buffer, uint32_t * bufferSize) = 0;
    virtual LoaderResultStatus ReadLogo(uint8_t * buffer, uint32_t * bufferSize) = 0;
    virtual LoaderResultStatus ReadProgramIds(uint64_t * buffer, uint32_t * count) = 0;
    virtual LoaderResultStatus Load(ISystemModules & modules, GuestProcessLoadParameters * out_parameters) = 0;
    virtual void AddToManualContentProvider(IManualContentProvider & provider) = 0;
    virtual void PrepareManualContent() = 0;
    virtual uint32_t GetGamePatches(GamePatchInfo * patches, uint32_t maxCount) = 0;
    // The add-on content found for the game, ordered by title id. Writes up
    // to maxCount entries and returns how many there are.
    virtual uint32_t GetAddOnContents(GameAddOnContentInfo * contents, uint32_t maxCount) = 0;
    virtual LoaderResultStatus ReadActiveUpdateVersion(char * buffer, uint32_t * bufferSize) = 0;
    virtual IFileSysNACP * GetBaseControlMetadata() = 0;
    virtual IFileSysNACP * GetControlMetadata() = 0;
    virtual IVirtualFile * ReadManualRomFS() = 0;
    virtual void Release() = 0;
};

// What a firmware installation is doing.
enum class FirmwareInstallStage : uint32_t
{
    Copying,   // the package's files, into a place beside the installed firmware
    Checking,  // that what was copied is a whole firmware
    Replacing, // the installed firmware with it
};

enum class FirmwareInstallStatus : uint32_t
{
    Installed,
    Cancelled,          // the observer stopped it; the installed firmware is untouched
    SourceUnreadable,   // the file or directory could not be read
    NotFirmware,        // nothing in the source is a firmware
    StorageUnavailable, // the emulated console's storage cannot be written
    CopyFailed,
    ReplaceFailed,      // the installed firmware was put back
};

// Follows a firmware installation. Called from the thread that installs.
nxinterface IFirmwareInstallObserver
{
    // done and total are in bytes while copying, in files while checking,
    // and 0 otherwise. Returning false stops the installation.
    virtual bool FirmwareInstallProgress(FirmwareInstallStage stage, uint64_t done, uint64_t total) = 0;
    // Asked when the installed firmware is at least as recent as the
    // package's. Returning false keeps the installed one.
    virtual bool ConfirmFirmwareReplacement(const char * packageVersion, const char * installedVersion) = 0;
};

// An already enumerated, complete library snapshot. A content file's
// signature is "size:mtime"; application IDs are base Title IDs.
struct GameContentFile
{
    std::string path;
    std::string signature;
    std::vector<uint64_t> applications;
};
struct GameContentSnapshot
{
    std::vector<std::string> directories;
    std::vector<GameContentFile> files;
    std::vector<std::string> modificationRoots;
};

nxinterface ISystemloader
{
    virtual bool Initialize() = 0;
    virtual uint32_t GetSupportedGameExtensions(const char ** extensions, uint32_t maxCount) const = 0;
    virtual bool LoadRom(const char * fileName, uint64_t selected_program_id, int32_t program_index, int32_t previous_program_index, ApplicationLaunchType launch_type) = 0;
    virtual IRomInfo * RomInfo(const char * fileName, uint64_t programId, uint64_t programIndex) = 0;
    virtual IRomInfo * FileRomInfo(IVirtualFile * file, uint64_t programId, uint64_t programIndex) = 0;
    virtual IRomInfo * LoadedRomInfo() = 0;

    virtual IContentProvider & ContentProvider() = 0;
    virtual IFilesystem & Filesystem() = 0;
    virtual IFileSystemController & FileSystemController() = 0;
    virtual IVirtualFile * SynthesizeSystemArchive(const uint64_t title_id) = 0;
    virtual IVirtualFile * CreateMemoryFile(const uint8_t * data, uint64_t size, const char * name) = 0;
    virtual uint32_t GetContentProviderEntriesCount(bool useTitleType, LoaderTitleType titleType, bool useContentRecordType, LoaderContentRecordType contentRecordType, bool useTitleId, unsigned long long titleId) = 0;
    virtual uint32_t GetContentProviderEntries(bool useTitleType, LoaderTitleType titleType, bool useContentRecordType, LoaderContentRecordType contentRecordType, bool useTitleId, unsigned long long titleId, ContentProviderEntry * entries, uint32_t entryCount) = 0;
    virtual IFileSysNCA * GetContentProviderEntry(uint64_t title_id, LoaderContentRecordType type) = 0;
    virtual IFileSysNACP * ReadControlData() = 0;
    virtual IFileSysNACP * GetPMControlMetadata(uint64_t programID) = 0;
    virtual IManualContentProvider & ManualContentProvider() = 0;
    virtual uint32_t GetInstalledFirmwareDisplayVersion(char * buffer, uint32_t buffer_size) const = 0;
    // The version of the firmware a game card image carries, when the console
    // has none or an older one: what installing it from the game would bring.
    // Returns its length, 0 when there is nothing to gain.
    virtual uint32_t GetGameCardFirmwareUpdate(const char * utf8_path, char * versionBuffer, uint32_t versionBufferSize) const = 0;
    // Installs the firmware a .zip, a .dxci or a directory of .nca files holds.
    // The installed firmware is only replaced once the new one is complete,
    // so a failure or a crash leaves it as it was. Nothing else may use the
    // loader meanwhile. observer may be null. versionBuffer receives the
    // package's version when one was read.
    virtual FirmwareInstallStatus InstallFirmwarePackage(const char * utf8_path, IFirmwareInstallObserver * observer, char * versionBuffer,
                                                         uint32_t versionBufferSize) = 0;
    virtual void SetDisabledAddons(uint64_t program_id, const char * const * names, uint32_t count) = 0;
    virtual bool IsAddonDisabled(uint64_t program_id, const char * name) const = 0;
    virtual LoaderResultStatus GetPMControlData(uint64_t program_id, uint8_t * buffer,
                                               uint32_t buffer_size, uint32_t & actual_size) = 0;
    // The applications a game container (.dnsp, .dxci, .nca) holds content
    // for, be it the game, an update or add-on content, as the program ids of
    // the base games. Writes up to max_count ids and returns how many there
    // are; none for a file that is not a container or cannot be read.
    virtual uint32_t GetContainerApplications(const char * utf8_path, uint64_t * application_ids, uint32_t max_count) = 0;
    // Publish a complete scan without a second filesystem traversal. A scan
    // of an obsolete directory configuration is rejected.
    virtual bool PublishGameContentSnapshot(const GameContentSnapshot & snapshot) = 0;
    // Refresh at launch, also when starting a file without opening the library.
    virtual void RefreshGameContentIndex() = 0;

};

EXPORT ISystemloader * CALL CreateSystemLoader(ISystemModules & modules);
EXPORT void CALL DestroySystemLoader(ISystemloader * systemloader);
