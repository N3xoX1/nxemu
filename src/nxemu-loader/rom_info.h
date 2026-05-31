#pragma once
#include "core/file_sys/vfs/vfs_types.h"
#include <memory>
#include <nxemu-loader/core/loader/loader.h>

class Systemloader;
nxinterface IManualContentProvider;

class RomInfo :
    public IRomInfo
{
public:
    explicit RomInfo(Systemloader & systemloader, FileSys::VirtualFile file, std::shared_ptr<Loader::AppLoader> loader);
    ~RomInfo();

    // IRomInfo
    LoaderFileType GetFileType() const override;
    LoaderResultStatus ReadProgramId(uint64_t & out_program_id) override;
    LoaderResultStatus ReadTitle(char * buffer, uint32_t * bufferSize) override;
    LoaderResultStatus ReadIcon(uint8_t * buffer, uint32_t * bufferSize) override;
    LoaderResultStatus ReadBanner(uint8_t * buffer, uint32_t * bufferSize) override;
    LoaderResultStatus ReadLogo(uint8_t * buffer, uint32_t * bufferSize) override;
    LoaderResultStatus ReadProgramIds(uint64_t * buffer, uint32_t * count) override;
    void AddToManualContentProvider(IManualContentProvider & provider) override;
    void Release() override;
    IVirtualFile * ReadManualRomFS() override;

private:
    RomInfo() = delete;
    RomInfo(const RomInfo &) = delete;
    RomInfo & operator=(const RomInfo &) = delete;

    Systemloader & m_systemloader;
    FileSys::VirtualFile m_file;
    std::shared_ptr<Loader::AppLoader> m_loader;
};