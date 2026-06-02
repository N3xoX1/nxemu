#include "rom_info.h"
#include "system_loader.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/content_archive.h"

#include "core/loader/loader.h"

RomInfo::RomInfo(Systemloader & systemloader, FileSys::VirtualFile file, std::shared_ptr<Loader::AppLoader> loader) :
    m_systemloader(systemloader),
    m_file(file),
    m_loader(std::move(loader))
{
}

RomInfo::~RomInfo()
{
}

LoaderFileType RomInfo::GetFileType() const
{
    if (m_loader)
    {
        return m_loader->GetFileType();
    }
    return LoaderFileType::Error;
}

LoaderResultStatus RomInfo::ReadProgramId(uint64_t & out_program_id)
{
    if (m_loader)
    {
        return m_loader->ReadProgramId(out_program_id);
    }
    return LoaderResultStatus::ErrorNotInitialized;
}

LoaderResultStatus RomInfo::ReadTitle(char * buffer, uint32_t * bufferSize)
{
    if (m_loader)
    {
        return m_loader->ReadTitle(buffer, bufferSize);
    }
    return LoaderResultStatus::ErrorNotInitialized;
}

LoaderResultStatus RomInfo::ReadIcon(uint8_t * buffer, uint32_t * bufferSize)
{
    if (m_loader)
    {
        return m_loader->ReadIcon(buffer, bufferSize);
    }
    return LoaderResultStatus::ErrorNotInitialized;
}

LoaderResultStatus RomInfo::ReadBanner(uint8_t * buffer, uint32_t * bufferSize)
{
    if (!m_loader)
    {
        return LoaderResultStatus::ErrorNotInitialized;
    }
    std::vector<u8> tmp;
    const LoaderResultStatus st = m_loader->ReadBanner(tmp);
    if (st != LoaderResultStatus::Success)
    {
        return st;
    }
    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    if (buffer != nullptr && *bufferSize < static_cast<uint32_t>(tmp.size()))
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = static_cast<uint32_t>(tmp.size());
    if (buffer == nullptr)
    {
        return LoaderResultStatus::Success;
    }
    std::memcpy(buffer, tmp.data(), tmp.size());
    return LoaderResultStatus::Success;
}

LoaderResultStatus RomInfo::ReadLogo(uint8_t * buffer, uint32_t * bufferSize)
{
    if (!m_loader)
    {
        return LoaderResultStatus::ErrorNotInitialized;
    }
    std::vector<u8> tmp;
    const LoaderResultStatus st = m_loader->ReadLogo(tmp);
    if (st != LoaderResultStatus::Success)
    {
        return st;
    }
    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    if (buffer != nullptr && *bufferSize < static_cast<uint32_t>(tmp.size()))
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = static_cast<uint32_t>(tmp.size());
    if (buffer == nullptr)
    {
        return LoaderResultStatus::Success;
    }
    std::memcpy(buffer, tmp.data(), tmp.size());
    return LoaderResultStatus::Success;
}

LoaderResultStatus RomInfo::ReadProgramIds(uint64_t * buffer, uint32_t * count)
{
    if (m_loader)
    {
        return m_loader->ReadProgramIds(buffer, count);
    }
    return LoaderResultStatus::ErrorNotInitialized;
}

void RomInfo::AddToManualContentProvider(IManualContentProvider & provider)
{
    const LoaderFileType file_type = GetFileType();
    if (file_type == LoaderFileType::NCA)
    {
        u64 program_id = 0;
        ReadProgramId(program_id);
        provider.AddEntry(LoaderTitleType::Application, FileSys::GetCRTypeFromNCAType(FileSys::NCA{m_file}.GetType()), program_id, std::make_unique<VirtualFileImpl>(m_file).release());
    }
    else if (file_type == LoaderFileType::XCI || file_type == LoaderFileType::NSP)
    {
        const std::shared_ptr<FileSys::NSP> nsp = file_type == LoaderFileType::NSP ? std::make_shared<FileSys::NSP>(m_file) : FileSys::XCI{m_file}.GetSecurePartitionNSP();
        for (const auto & title : nsp->GetNCAs())
        {
            for (const auto & entry : title.second)
            {
                FileSys::VirtualFile file = entry.second->BaseFile();
                provider.AddEntry(entry.first.first, entry.first.second, title.first, std::make_unique<VirtualFileImpl>(file).release());
            }
        }
    }
}

IVirtualFile * RomInfo::ReadManualRomFS()
{
    if (!m_loader)
    {
        return nullptr;
    }
    FileSys::VirtualFile out;
    if (m_loader->ReadManualRomFS(out) != LoaderResultStatus::Success || !out)
    {
        return nullptr;
    }
    return std::make_unique<VirtualFileImpl>(out).release();
}

void RomInfo::Release()
{
    delete this;
}