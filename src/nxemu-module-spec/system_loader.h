#pragma once
#include "base.h"
#include "operating_system.h"

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
    virtual bool Resize(uint64_t size) = 0;
    virtual uint64_t ReadBytes(uint8_t * data, uint64_t datalen, uint64_t offset) = 0;
    virtual uint64_t WriteBytes(const uint8_t * data, uint64_t datalen, uint64_t offset) = 0;
    virtual IVirtualDirectory * ExtractRomFS() = 0;
    virtual IVirtualDirectory * GetContainingDirectory() const = 0;
    virtual IVirtualFile * Duplicate() = 0;
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
    virtual bool OpenSaveData(IVirtualDirectory ** out_save_data, SaveDataSpaceId space, const SaveDataAttribute & attribute) = 0;
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
    virtual bool GetUserAccountSwitchLock() const = 0;
    virtual bool GetRatingAge(uint8_t * buffer, uint32_t bufferSize) const = 0;
    virtual const char * GetVersionString() const = 0;
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
};

nxinterface IContentProvider
{
    virtual IFileSysNCA * GetEntry(uint64_t title_id, LoaderContentRecordType type) const = 0;
};

nxinterface IManualContentProvider
{
    virtual void AddEntry(LoaderTitleType title_type, LoaderContentRecordType content_type, uint64_t title_id, IVirtualFile * file) = 0;
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
    virtual LoaderResultStatus ReadIcon(uint8_t * buffer, uint32_t * bufferSize) = 0;
    virtual LoaderResultStatus ReadBanner(uint8_t * buffer, uint32_t * bufferSize) = 0;
    virtual LoaderResultStatus ReadLogo(uint8_t * buffer, uint32_t * bufferSize) = 0;
    virtual LoaderResultStatus ReadProgramIds(uint64_t * buffer, uint32_t * count) = 0;
    virtual void AddToManualContentProvider(IManualContentProvider & provider) = 0;
    virtual IVirtualFile * ReadManualRomFS() = 0;
    virtual void Release() = 0;
};

nxinterface ISystemloader
{
    virtual bool Initialize() = 0;
    virtual bool SelectAndLoad(void * parentWindow) = 0;
    virtual bool LoadRom(const char * fileName) = 0;
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
    virtual bool InstallFirmwarePackage(const char * utf8_path) = 0;
};

EXPORT ISystemloader * CALL CreateSystemLoader(ISystemModules & modules);
EXPORT void CALL DestroySystemLoader(ISystemloader * systemloader);
