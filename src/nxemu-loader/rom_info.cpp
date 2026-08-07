#include "rom_info.h"
#include "system_loader.h"
#include "loader_settings.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/patch_manager.h"
#include "core/loader/loader.h"
#include <algorithm>
#include <common/path.h>
#include <filesystem>
#include <map>
#include <yuzu_common/fs/fs.h>

namespace
{

using UpdateVersionMap = std::map<uint64_t, uint32_t>;

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

bool IsAddOnContentExtension(const char * fileName)
{
    static const char * supported_extensions[] = {
        "nca",
        "dxci",
        "dnsp",
    };

    std::string ext = Path(fileName).GetExtension();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    for (const char * supported : supported_extensions)
    {
        if (ext == supported)
        {
            return true;
        }
    }
    return false;
}

void CollectUpdateVersions(const FileSys::NSP & nsp, UpdateVersionMap & versions)
{
    using TitleNcaMap =
        std::map<std::pair<LoaderTitleType, LoaderContentRecordType>, std::shared_ptr<FileSys::NCA>>;
    using NcaCollection = std::map<uint64_t, TitleNcaMap>;

    for (const NcaCollection::value_type & title : nsp.GetNCAs())
    {
        for (const TitleNcaMap::value_type & entry : title.second)
        {
            if (!entry.second || entry.first.second != LoaderContentRecordType::Meta)
            {
                continue;
            }

            const std::vector<FileSys::VirtualDir> subdirs = entry.second->GetSubdirectories();
            if (subdirs.empty())
            {
                continue;
            }

            for (const FileSys::VirtualFile & inner_file : subdirs[0]->GetFiles())
            {
                if (inner_file->GetExtension() == "cnmt")
                {
                    const FileSys::CNMT cnmt(inner_file);
                    versions[cnmt.GetTitleID()] = cnmt.GetTitleVersion();
                    break;
                }
            }
        }
    }
}

void RegisterMatchingAddOnEntriesFromNsp(const FileSys::NSP & nsp, uint64_t base_program_id, IManualContentProvider & provider)
{
    if (nsp.GetStatus() != LoaderResultStatus::Success)
    {
        return;
    }

    UpdateVersionMap versions;
    CollectUpdateVersions(nsp, versions);

    for (const auto & title : nsp.GetNCAs())
    {
        if (FileSys::GetBaseTitleID(title.first) != base_program_id)
        {
            continue;
        }

        for (const auto & entry : title.second)
        {
            if (entry.first.first != LoaderTitleType::Update && entry.first.first != LoaderTitleType::AOC)
            {
                continue;
            }

            FileSys::VirtualFile file = entry.second->BaseFile();
            if (!file)
            {
                continue;
            }

            uint32_t version = 0;
            if (entry.first.first == LoaderTitleType::Update)
            {
                if (const auto it = versions.find(title.first); it != versions.end())
                {
                    version = it->second;
                }
            }
            provider.AddEntry(entry.first.first, entry.first.second, title.first, version, std::make_unique<VirtualFileImpl>(file).release());
        }
    }
}

void RegisterConfiguredAddOnDirectoryEntries(uint64_t program_id, FileSys::VirtualFilesystem & vfs, IManualContentProvider & provider)
{
    if (program_id == 0 || loaderSettings.addOnDirectories.empty())
    {
        return;
    }

    const uint64_t base_program_id = FileSys::GetBaseTitleID(program_id);

    for (const std::string & dir : loaderSettings.addOnDirectories)
    {
        if (dir.empty())
        {
            continue;
        }

        const auto callback = [&](const std::filesystem::directory_entry & entry) -> bool {
            const std::filesystem::path & path = entry.path();
            if (!Common::FS::IsFile(path))
            {
                return true;
            }
            const std::string physical_name = Common::FS::PathToUTF8String(path);
            if (!IsAddOnContentExtension(physical_name.c_str()))
            {
                return true;
            }

            FileSys::VirtualFile file = vfs->OpenFile(physical_name, VirtualFileOpenMode::Read);
            if (!file)
            {
                return true;
            }

            const std::string ext = ToLowerAscii(std::string(Path(physical_name).GetExtension()));
            if (ext == "dnsp")
            {
                RegisterMatchingAddOnEntriesFromNsp(FileSys::NSP(file), base_program_id, provider);
            }
            else if (ext == "dxci")
            {
                const FileSys::XCI xci(file);
                if (xci.GetStatus() == LoaderResultStatus::Success)
                {
                    const std::shared_ptr<FileSys::NSP> nsp = xci.GetSecurePartitionNSP();
                    if (nsp)
                    {
                        RegisterMatchingAddOnEntriesFromNsp(*nsp, base_program_id, provider);
                    }
                }
            }
            else if (ext == "nca")
            {
                const FileSys::NCA nca(file);
                if (nca.GetStatus() != LoaderResultStatus::Success)
                {
                    return true;
                }

                const uint64_t title_id = nca.GetTitleId();
                if (FileSys::GetBaseTitleID(title_id) != base_program_id || title_id == base_program_id)
                {
                    return true;
                }

                const LoaderTitleType title_type = (title_id & 0x800) != 0 ? LoaderTitleType::Update : LoaderTitleType::AOC;
                provider.AddEntry(title_type, FileSys::GetCRTypeFromNCAType(nca.GetType()), title_id, 0, std::make_unique<VirtualFileImpl>(file).release());
            }
            return true;
        };

        Common::FS::IterateDirEntriesRecursively(dir, callback, Common::FS::DirEntryFilter::File);
    }
}

} // namespace

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

LoaderResultStatus RomInfo::Load(ISystemModules & modules, GuestProcessLoadParameters * out_parameters)
{
    if (!m_loader)
    {
        return LoaderResultStatus::ErrorNotInitialized;
    }

    const auto [status, params] = m_loader->Load(m_systemloader, modules);
    if (status == LoaderResultStatus::Success && out_parameters && params)
    {
        out_parameters->main_thread_priority = params->main_thread_priority;
        out_parameters->main_thread_stack_size = (int64_t)params->main_thread_stack_size;
    }
    return status;
}

void RomInfo::AddToManualContentProvider(IManualContentProvider & provider)
{
    const LoaderFileType file_type = GetFileType();
    if (file_type == LoaderFileType::NCA)
    {
        u64 program_id = 0;
        ReadProgramId(program_id);
        provider.AddEntry(LoaderTitleType::Application, FileSys::GetCRTypeFromNCAType(FileSys::NCA{m_file}.GetType()), program_id, 0, std::make_unique<VirtualFileImpl>(m_file).release());
    }
    else if (file_type == LoaderFileType::XCI || file_type == LoaderFileType::NSP)
    {
        const std::shared_ptr<FileSys::NSP> nsp = file_type == LoaderFileType::NSP ? std::make_shared<FileSys::NSP>(m_file) : FileSys::XCI{m_file}.GetSecurePartitionNSP();
        if (!nsp)
        {
            return;
        }

        UpdateVersionMap versions;
        CollectUpdateVersions(*nsp, versions);

        for (const auto & title : nsp->GetNCAs())
        {
            for (const auto & entry : title.second)
            {
                FileSys::VirtualFile file = entry.second->BaseFile();
                if (!file)
                {
                    continue;
                }

                uint32_t version = 0;
                if (entry.first.first == LoaderTitleType::Update)
                {
                    if (const auto it = versions.find(title.first); it != versions.end())
                    {
                        version = it->second;
                    }
                }
                provider.AddEntry(entry.first.first, entry.first.second, title.first, version, std::make_unique<VirtualFileImpl>(file).release());
            }
        }
    }
}

void RomInfo::PrepareManualContent()
{
    IManualContentProvider & provider = m_systemloader.ManualContentProvider();
    AddToManualContentProvider(provider);

    uint64_t program_id = 0;
    ReadProgramId(program_id);
    FileSys::VirtualFilesystem vfs = m_systemloader.GetFilesystem();
    RegisterConfiguredAddOnDirectoryEntries(program_id, vfs, provider);
}

uint32_t RomInfo::GetGamePatches(GamePatchInfo * patches, uint32_t maxCount)
{
    uint64_t program_id = 0;
    if (ReadProgramId(program_id) != LoaderResultStatus::Success || program_id == 0)
    {
        return 0;
    }

    const FileSys::PatchManager pm(program_id, m_systemloader.GetFileSystemController(), m_systemloader.GetContentProvider());
    const std::vector<FileSys::Patch> list = pm.GetPatches();
    if (patches == nullptr || maxCount == 0)
    {
        return (uint32_t)list.size();
    }

    const uint32_t count = std::min(maxCount, (uint32_t)list.size());
    for (uint32_t i = 0; i < count; ++i)
    {
        patches[i].enabled = list[i].enabled;
        std::snprintf(patches[i].name, sizeof(patches[i].name), "%s", list[i].name.c_str());
        std::snprintf(patches[i].version, sizeof(patches[i].version), "%s", list[i].version.c_str());
        if (list[i].type == FileSys::PatchType::Update && list[i].title_id != list[i].program_id)
        {
            std::snprintf(patches[i].key, sizeof(patches[i].key), "Update@%u", list[i].numeric_version);
        }
        else
        {
            std::snprintf(patches[i].key, sizeof(patches[i].key), "%s", list[i].name.c_str());
        }
    }
    return count;
}

IFileSysNACP * RomInfo::GetControlMetadata()
{
    uint64_t program_id = 0;
    if (ReadProgramId(program_id) != LoaderResultStatus::Success || program_id == 0)
    {
        return nullptr;
    }
    return m_systemloader.GetPMControlMetadata(program_id);
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