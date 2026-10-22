#include "rom_info.h"
#include "system_loader.h"
#include "loader_settings.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/submission_package.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/nca_metadata.h"
#include "core/loader/loader.h"
#include <algorithm>
#include <common/path.h>
#include <cstring>
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
            provider.AddEntry(entry.first.first, entry.first.second, title.first, version,
                              std::make_unique<VirtualFileImpl>(file).release());
        }
    }
}

void RegisterMatchingAddOnEntriesFromFile(FileSys::VirtualFile file,
    uint64_t base_program_id, IManualContentProvider & provider)
{
    if (!file)
    {
        return;
    }

    const std::string ext = ToLowerAscii(file->GetExtension());
    if (ext == "dnsp")
    {
        RegisterMatchingAddOnEntriesFromNsp(FileSys::NSP(file, 0, 0, base_program_id), base_program_id, provider);
    }
    else if (ext == "dxci")
    {
        const FileSys::XCI xci(file, 0, 0, base_program_id);
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
        if (nca.GetStatus() != LoaderResultStatus::Success &&
            nca.GetStatus() != LoaderResultStatus::ErrorMissingBKTRBaseRomFS)
        {
            return;
        }

        const uint64_t title_id = nca.GetTitleId();
        if (FileSys::GetBaseTitleID(title_id) != base_program_id ||
            title_id == base_program_id)
        {
            return;
        }

        const LoaderTitleType title_type =
            (title_id & 0x800) != 0 ? LoaderTitleType::Update : LoaderTitleType::AOC;
        provider.AddEntry(title_type, FileSys::GetCRTypeFromNCAType(nca.GetType()), title_id, 0,
                          std::make_unique<VirtualFileImpl>(file).release());
    }
}

void RegisterSiblingAddOnEntries(uint64_t program_id, const FileSys::VirtualFile & game_file,
                                 IManualContentProvider & provider)
{
    if (program_id == 0 || !game_file)
    {
        return;
    }

    const FileSys::VirtualDir parent = game_file->GetContainingDirectory();
    if (!parent)
    {
        return;
    }

    const uint64_t base_program_id = FileSys::GetBaseTitleID(program_id);
    for (const FileSys::VirtualFile & sibling : parent->GetFiles())
    {
        if (!sibling || sibling->GetName() == game_file->GetName())
        {
            continue;
        }

        const std::string ext = ToLowerAscii(sibling->GetExtension());
        if (ext != "dnsp" && ext != "dxci" && ext != "nca")
        {
            continue;
        }

        RegisterMatchingAddOnEntriesFromFile(sibling, base_program_id, provider);
    }
}

void RegisterConfiguredGameDirectoryEntries(uint64_t program_id, const FileSys::VirtualFile & game_file,
                                            Systemloader & loader, IManualContentProvider & provider)
{
    if (program_id == 0) return;
    const uint64_t base_id = FileSys::GetBaseTitleID(program_id);
    FileSys::VirtualFilesystem vfs = loader.GetFilesystem();
    if (!vfs) return;
    const std::string current_game = game_file ? game_file->GetFullPath() : std::string();
    for (const std::string & path : loader.GameContentFiles(base_id))
    {
        if (path == current_game) continue;
        FileSys::VirtualFile file = vfs->OpenFile(path, VirtualFileOpenMode::Read);
        if (file) RegisterMatchingAddOnEntriesFromFile(file, base_id, provider);
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

LoaderResultStatus RomInfo::ReadDisplayVersion(char * buffer, uint32_t * bufferSize)
{
    if (!m_loader)
    {
        return LoaderResultStatus::ErrorNotInitialized;
    }
    FileSys::NACP nacp;
    const LoaderResultStatus st = m_loader->ReadControlData(nacp);
    if (st != LoaderResultStatus::Success)
    {
        return st;
    }
    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    const std::string version = nacp.GetVersionString();
    if (buffer != nullptr && *bufferSize < (uint32_t)version.size())
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = (uint32_t)version.size();
    if (buffer == nullptr)
    {
        return LoaderResultStatus::Success;
    }
    std::memcpy(buffer, version.data(), version.size());
    return LoaderResultStatus::Success;
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
    PrepareManualContentFor(m_systemloader.ManualContentProvider());
}

void RomInfo::PrepareManualContentFor(IManualContentProvider & provider)
{
    AddToManualContentProvider(provider);

    uint64_t program_id = 0;
    ReadProgramId(program_id);

    // One indexed lookup across all configured game directories, independent
    // of their order. Only matching content files are opened for this game.
    RegisterConfiguredGameDirectoryEntries(program_id, m_file, m_systemloader, provider);

    // Preserve sibling discovery for ROMs launched outside configured folders.
    // Avoid registering the same update twice when a root is configured.
    if (m_file && !m_systemloader.IsInGameDirectory(m_file->GetFullPath()))
        RegisterSiblingAddOnEntries(program_id, m_file, provider);
}

std::vector<FileSys::Patch> RomInfo::GetPatchesForThisRom()
{
    uint64_t program_id = 0;
    if (ReadProgramId(program_id) != LoaderResultStatus::Success || program_id == 0)
    {
        return {};
    }

    // A library scan can run while another game is loaded. Keep its manually
    // discovered updates separate from the provider used by the running game.
    ManualContentProviderImpl manual_provider;
    PrepareManualContentFor(manual_provider);
    FileSys::ContentProviderUnion content_provider =
        static_cast<FileSys::ContentProviderUnion &>(m_systemloader.GetContentProvider());
    content_provider.SetSlot(FileSys::ContentProviderUnionSlot::FrontendManual, &manual_provider.Provider());
    const FileSys::PatchManager pm(program_id, m_systemloader.GetFileSystemController(), content_provider);
    return pm.GetPatches();
}

uint32_t RomInfo::GetGamePatches(GamePatchInfo * patches, uint32_t maxCount)
{
    const std::vector<FileSys::Patch> list = GetPatchesForThisRom();
    if (patches == nullptr || maxCount == 0)
    {
        return (uint32_t)list.size();
    }

    const uint32_t count = std::min(maxCount, (uint32_t)list.size());
    for (uint32_t i = 0; i < count; ++i)
    {
        patches[i].enabled = list[i].enabled;
        patches[i].type = list[i].type == FileSys::PatchType::Update ? GamePatchType::Update :
                          list[i].type == FileSys::PatchType::DLC ? GamePatchType::AddOnContent : GamePatchType::Mod;
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

uint32_t RomInfo::GetAddOnContents(GameAddOnContentInfo * contents, uint32_t maxCount)
{
    uint64_t program_id = 0;
    if (ReadProgramId(program_id) != LoaderResultStatus::Success || program_id == 0)
    {
        return 0;
    }

    // As GetPatchesForThisRom(): what is found for this game is kept apart
    // from the provider of a running one.
    ManualContentProviderImpl manual_provider;
    PrepareManualContentFor(manual_provider);
    FileSys::ContentProviderUnion content_provider =
        static_cast<FileSys::ContentProviderUnion &>(m_systemloader.GetContentProvider());
    content_provider.SetSlot(FileSys::ContentProviderUnionSlot::FrontendManual, &manual_provider.Provider());

    std::vector<GameAddOnContentInfo> list;
    for (const ContentProviderEntry & entry : content_provider.ListEntriesFilter(LoaderTitleType::AOC, LoaderContentRecordType::Data, std::nullopt))
    {
        if (FileSys::GetBaseTitleID(entry.titleID) != program_id ||
            std::any_of(list.begin(), list.end(), [&entry](const GameAddOnContentInfo & known) { return known.titleId == entry.titleID; }))
        {
            continue;
        }
        const std::unique_ptr<FileSys::NCA> nca = content_provider.GetEntryNCA(entry.titleID, entry.type);
        if (nca == nullptr || nca->GetStatus() != LoaderResultStatus::Success)
        {
            continue;
        }
        const FileSys::VirtualFile file = nca->BaseFile();
        list.push_back({entry.titleID, content_provider.GetEntryVersion(entry.titleID).value_or(0), file != nullptr ? file->GetSize() : 0});
    }
    std::sort(list.begin(), list.end(), [](const GameAddOnContentInfo & left, const GameAddOnContentInfo & right) { return left.titleId < right.titleId; });
    if (contents == nullptr || maxCount == 0)
    {
        return static_cast<uint32_t>(list.size());
    }
    const uint32_t count = std::min(maxCount, static_cast<uint32_t>(list.size()));
    std::copy_n(list.begin(), count, contents);
    return count;
}

LoaderResultStatus RomInfo::ReadActiveUpdateVersion(char * buffer, uint32_t * bufferSize)
{
    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }

    std::string version;
    for (const FileSys::Patch & patch : GetPatchesForThisRom())
    {
        if (patch.type == FileSys::PatchType::Update && patch.enabled)
        {
            version = patch.version;
            break;
        }
    }
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V'))
    {
        version.erase(0, 1);
    }

    const uint32_t required = static_cast<uint32_t>(version.size() + 1);
    if (buffer != nullptr && *bufferSize < required)
    {
        *bufferSize = required;
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = required;
    if (buffer != nullptr)
    {
        std::memcpy(buffer, version.c_str(), required);
    }
    return LoaderResultStatus::Success;
}

IFileSysNACP * RomInfo::GetControlMetadata()
{
    uint64_t program_id = 0;
    if (ReadProgramId(program_id) == LoaderResultStatus::Success && program_id != 0)
    {
        if (IFileSysNACP * metadata = m_systemloader.GetPMControlMetadata(program_id))
        {
            return metadata;
        }
    }
    return GetBaseControlMetadata();
}

IFileSysNACP * RomInfo::GetBaseControlMetadata()
{
    FileSys::NACP nacp;
    if (m_loader && m_loader->ReadControlData(nacp) == LoaderResultStatus::Success)
    {
        return new FileSys::NACP(nacp);
    }
    return nullptr;
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
