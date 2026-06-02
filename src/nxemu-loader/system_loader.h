#pragma once
#include <nxemu-module-spec/system_loader.h>
#include <memory>
#include "core/file_sys/vfs/vfs_types.h"
#include "core/file_sys/registered_cache.h"

class FileSystemController;

namespace FileSys
{
class ContentProvider;
}

class Systemloader :
    public ISystemloader
{
public:
    Systemloader(ISystemModules & modules);
    ~Systemloader();

    FileSys::ContentProvider & GetContentProvider();
    FileSys::VirtualFilesystem GetFilesystem();
    ::FileSystemController & GetFileSystemController();
    void RegisterContentProvider(FileSys::ContentProviderUnionSlot slot, FileSys::ContentProvider* provider);

    void SetProcessID(uint64_t processID);
    void SetTitleID(uint64_t titleID);
    
    //ISystemloader
    bool Initialize() override;
    bool SelectAndLoad(void * parentWindow) override;
    bool LoadRom(const char * fileName) override;
    IRomInfo * RomInfo(const char * fileName, uint64_t programId, uint64_t programIndex) override;
    IRomInfo * FileRomInfo(IVirtualFile * file, uint64_t programId, uint64_t programIndex) override;
    IRomInfo * LoadedRomInfo() override;

    IFileSystemController & FileSystemController() override;
    IContentProvider & ContentProvider() override;
    IFilesystem & Filesystem() override;
    IVirtualFile * SynthesizeSystemArchive(const uint64_t title_id) override;
    IVirtualFile * CreateMemoryFile(const uint8_t * data, uint64_t size, const char * name) override;
    uint32_t GetContentProviderEntriesCount(bool useTitleType, LoaderTitleType titleType, bool useContentRecordType, LoaderContentRecordType contentRecordType, bool useTitleId, unsigned long long titleId) override;
    uint32_t GetContentProviderEntries(bool useTitleType, LoaderTitleType titleType, bool useContentRecordType, LoaderContentRecordType contentRecordType, bool useTitleId, unsigned long long titleId, ContentProviderEntry * entries, uint32_t entryCount) override;
    IFileSysNCA * GetContentProviderEntry(uint64_t title_id, LoaderContentRecordType type) override;
    IFileSysNACP * ReadControlData() override;
    IFileSysNACP * GetPMControlMetadata(uint64_t programID) override;
    IManualContentProvider & ManualContentProvider() override;
    uint32_t GetInstalledFirmwareDisplayVersion(char * buffer, uint32_t buffer_size) const override;
    bool InstallFirmwarePackage(const char * utf8_path) override;

private:
    Systemloader() = delete;
    Systemloader(const Systemloader &) = delete;
    Systemloader & operator=(const Systemloader &) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl;
};
