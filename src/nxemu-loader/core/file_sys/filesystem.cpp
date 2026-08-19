// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_sys/filesystem.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/save_data_controller.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/bis_factory.h"
#include "core/file_sys/sdmc_factory.h"
#include "core/file_sys/romfs_factory.h"

#include "loader_settings.h"
#include "system_loader.h"
#include <unordered_set>
#include <yuzu_common/logging/log.h>
#include <yuzu_common/fs/fs.h>
#include <yuzu_common/fs/path_util.h>

namespace
{
void CollectTitleIdModRoots(const std::filesystem::path & path, const std::string & title_id_hex, FileSys::VfsFilesystem & vfs, std::vector<FileSys::VirtualDir> & out, std::unordered_set<std::string> & seen_paths)
{
    if (!Common::FS::IsDir(path))
    {
        return;
    }

    const std::string folder_name = Common::FS::PathToUTF8String(path.filename());
    bool TitleIdFolderNameEquals = folder_name.size() == title_id_hex.size();
    if (TitleIdFolderNameEquals)
    {
        for (size_t i = 0; i < folder_name.size(); ++i)
        {
            if (std::tolower((unsigned char)folder_name[i]) != std::tolower((unsigned char)title_id_hex[i]))
            {
                TitleIdFolderNameEquals = false;
                break;
            }
        }
    }

    if (TitleIdFolderNameEquals)
    {
        const std::string utf8 = Common::FS::PathToUTF8String(path);
        FileSys::VirtualDir dir = vfs.OpenDirectory(utf8, VirtualFileOpenMode::Read);
        if (dir != nullptr && seen_paths.insert(dir->GetFullPath()).second)
        {
            out.push_back(std::move(dir));
        }
        return;
    }

    std::error_code ec;
    for (const auto & entry : std::filesystem::directory_iterator(path, ec))
    {
        std::error_code type_ec;
        if (ec || !entry.is_directory(type_ec) || type_ec)
        {
            continue;
        }
        CollectTitleIdModRoots(entry.path(), title_id_hex, vfs, out, seen_paths);
    }
}

} // namespace

FileSystemController::FileSystemController(Systemloader & loader_) :
    loader(loader_)
{
}

FileSystemController::~FileSystemController() = default;

bool FileSystemController::RegisterProcess(FileSys::ProcessId process_id, FileSys::ProgramId program_id, std::shared_ptr<FileSys::RomFSFactory>&& romfs_factory)
{
    std::scoped_lock lk{registration_lock};

    registrations.emplace(process_id, Registration{.program_id = program_id, .romfs_factory = std::move(romfs_factory), .save_data_factory = CreateSaveDataFactory(program_id), });

    LOG_DEBUG(Service_FS, "Registered for process {}", process_id);
    return true;
}

void FileSystemController::SetPackedUpdate(FileSys::ProcessId process_id, FileSys::VirtualFile update_raw)
{
    LOG_TRACE(Service_FS, "Setting packed update for romfs");

    std::scoped_lock lk{registration_lock};
    const auto it = registrations.find(process_id);
    if (it == registrations.end())
    {
        return;
    }
    it->second.romfs_factory->SetPackedUpdate(std::move(update_raw));
}

ISaveDataController * FileSystemController::OpenSaveDataController() const
{
    std::shared_ptr<SaveDataController> dataController(std::make_shared<SaveDataController>(loader, CreateSaveDataFactory(FileSys::ProgramId{})));
    return std::make_unique<SaveDataControllerPtr>(dataController).release();
}

IFileSysRegisteredCache & FileSystemController::GetSystemNANDContents() const
{
    return *SystemNANDContents();
}

uint64_t FileSystemController::GetFreeSpaceSize(StorageId id) const
{
    UNIMPLEMENTED();
    return 0;
}

uint64_t FileSystemController::GetTotalSpaceSize(StorageId id) const
{
    UNIMPLEMENTED();
    return 0;
}

bool FileSystemController::OpenProcess(uint64_t * programId, ISaveDataFactory ** saveDataFactory, IRomFsController ** romFsController, uint64_t processId)
{
    std::scoped_lock lk{registration_lock};

    const auto it = registrations.find(processId);
    if (it == registrations.end())
    {
        UNIMPLEMENTED();
        return false;
    }

    *programId = it->second.program_id;
    *saveDataFactory = std::make_unique<SaveDataFactoryImpl>(it->second.save_data_factory).release();
    *romFsController = std::make_unique<RomFsControllerImpl>(loader, it->second.romfs_factory, it->second.program_id).release();
    return true;
}

bool FileSystemController::OpenSDMC(IVirtualDirectory ** out_sdmc) const
{
    LOG_TRACE(Service_FS, "Opening SDMC");

    if (sdmc_factory == nullptr)
    {
        return false;
    }

    FileSys::VirtualDir sdmc = sdmc_factory->Open();
    if (sdmc == nullptr)
    {
        return false;
    }

    *out_sdmc = std::make_unique<VirtualDirectoryImpl>(sdmc).release();
    return true;
}

FileSys::RegisteredCache * FileSystemController::SystemNANDContents() const
{
    LOG_TRACE(Service_FS, "Opening System NAND Contents");

    if (bis_factory == nullptr)
    {
        return nullptr;
    }
    return bis_factory->GetSystemNANDContents();
}

FileSys::RegisteredCache * FileSystemController::UserNANDContents() const
{
    LOG_TRACE(Service_FS, "Opening User NAND Contents");

    if (bis_factory == nullptr)
    {
        return nullptr;
    }

    return bis_factory->GetUserNANDContents();
}

FileSys::RegisteredCache * FileSystemController::SDMCContents() const
{
    LOG_TRACE(Service_FS, "Opening SDMC Contents");

    if (sdmc_factory == nullptr)
    {
        return nullptr;
    }
    return sdmc_factory->GetSDMCContents();
}

FileSys::VirtualDir FileSystemController::GetModificationLoadRoot(uint64_t title_id) const
{
    LOG_TRACE(Service_FS, "Opening mod load root for tid={:016X}", title_id);

    if (bis_factory == nullptr)
    {
        return nullptr;
    }
    return bis_factory->GetModificationLoadRoot(title_id);
}

FileSys::VirtualDir FileSystemController::GetSDMCModificationLoadRoot(uint64_t title_id) const
{
    LOG_TRACE(Service_FS, "Opening SDMC mod load root for tid={:016X}", title_id);

    if (sdmc_factory == nullptr)
    {
        return nullptr;
    }
    return sdmc_factory->GetSDMCModificationLoadRoot(title_id);
}

std::vector<FileSys::VirtualDir> FileSystemController::GetAddOnModificationLoadRoots(uint64_t title_id) const
{
    std::vector<FileSys::VirtualDir> roots;
    if (title_id == 0 || (title_id & 0xFFF) == 0x800 || loaderSettings.addOnDirectories.empty())
    {
        return roots;
    }

    FileSys::VirtualFilesystem vfs = loader.GetFilesystem();
    if (!vfs)
    {
        return roots;
    }

    const std::string title_id_hex = fmt::format("{:016X}", title_id);
    std::unordered_set<std::string> seen_paths;
    for (const std::string & dir : loaderSettings.addOnDirectories)
    {
        if (dir.empty())
        {
            continue;
        }
        CollectTitleIdModRoots(std::filesystem::path(Common::FS::ToU8String(dir)), title_id_hex, *vfs, roots, seen_paths);
    }
    return roots;
}

FileSys::VirtualDir FileSystemController::GetModificationDumpRoot(uint64_t title_id) const
{
    LOG_TRACE(Service_FS, "Opening mod dump root for tid={:016X}", title_id);

    if (bis_factory == nullptr)
    {
        return nullptr;
    }
    return bis_factory->GetModificationDumpRoot(title_id);
}

FileSys::VirtualDir FileSystemController::GetSystemNANDContentDirectory() const
{
    if (bis_factory == nullptr)
    {
        return nullptr;
    }
    return bis_factory->GetSystemNANDContentDirectory();
}

void FileSystemController::CreateFactories(FileSys::VfsFilesystem & vfs, bool overwrite)
{
    if (overwrite)
    {
        bis_factory = nullptr;
        sdmc_factory = nullptr;
    }

    using YuzuPath = Common::FS::YuzuPath;
    const auto sdmc_dir_path = Common::FS::GetYuzuPath(YuzuPath::SDMCDir);
    const auto sdmc_load_dir_path = sdmc_dir_path / "atmosphere/contents";
    const auto rw_mode = VirtualFileOpenMode::ReadWrite;

    auto nand_directory = vfs.OpenDirectory(Common::FS::GetYuzuPathString(YuzuPath::NANDDir), rw_mode);
    auto sd_directory = vfs.OpenDirectory(Common::FS::PathToUTF8String(sdmc_dir_path), rw_mode);
    auto load_directory = vfs.OpenDirectory(Common::FS::GetYuzuPathString(YuzuPath::LoadDir), VirtualFileOpenMode::Read);
    auto sd_load_directory = vfs.OpenDirectory(Common::FS::PathToUTF8String(sdmc_load_dir_path), VirtualFileOpenMode::Read);
    auto dump_directory = vfs.OpenDirectory(Common::FS::GetYuzuPathString(YuzuPath::DumpDir), rw_mode);

    if (bis_factory == nullptr)
    {
        bis_factory = std::make_unique<FileSys::BISFactory>(nand_directory, std::move(load_directory), std::move(dump_directory));
        loader.RegisterContentProvider(FileSys::ContentProviderUnionSlot::SysNAND, bis_factory->GetSystemNANDContents());
        loader.RegisterContentProvider(FileSys::ContentProviderUnionSlot::UserNAND, bis_factory->GetUserNANDContents());
    }

    if (sdmc_factory == nullptr)
    {
        sdmc_factory = std::make_unique<FileSys::SDMCFactory>(std::move(sd_directory), std::move(sd_load_directory));
        loader.RegisterContentProvider(FileSys::ContentProviderUnionSlot::SDMC, sdmc_factory->GetSDMCContents());
    }
}

std::shared_ptr<FileSys::SaveDataFactory> FileSystemController::CreateSaveDataFactory(FileSys::ProgramId program_id) const
{
    using YuzuPath = Common::FS::YuzuPath;
    const auto rw_mode = VirtualFileOpenMode::ReadWrite;

    auto vfs = loader.GetFilesystem();
    const auto nand_directory = vfs->OpenDirectory(Common::FS::GetYuzuPathString(YuzuPath::NANDDir), rw_mode);
    return std::make_shared<FileSys::SaveDataFactory>(program_id, std::move(nand_directory));
}
