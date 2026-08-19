// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "core/file_sys/vfs/vfs_types.h"
#include <yuzu_common/common_types.h>
#include <nxemu-module-spec/system_loader.h>

class Systemloader;

namespace FileSys {
class RomFSFactory;
class SaveDataFactory;
class SDMCFactory;
class BISFactory;
class VfsFilesystem;
class RegisteredCache;

using ProcessId = uint64_t;
using ProgramId = uint64_t;

} // namespace FileSys

class FileSystemController :
    public IFileSystemController
{
public:
    explicit FileSystemController(Systemloader & loader_);
    ~FileSystemController();

    bool RegisterProcess(FileSys::ProcessId process_id, FileSys::ProgramId program_id, std::shared_ptr<FileSys::RomFSFactory>&& romfs_factory);
    void SetPackedUpdate(FileSys::ProcessId process_id, FileSys::VirtualFile update_raw);

    FileSys::RegisteredCache * SystemNANDContents() const;
    FileSys::RegisteredCache * UserNANDContents() const;
    FileSys::RegisteredCache * SDMCContents() const;
    FileSys::RegisteredCache * GameCardContents() const;

    FileSys::VirtualDir GetSDMCModificationLoadRoot(uint64_t title_id) const;
    FileSys::VirtualDir GetModificationLoadRoot(uint64_t title_id) const;
    FileSys::VirtualDir GetModificationDumpRoot(uint64_t title_id) const;
    std::vector<FileSys::VirtualDir> GetAddOnModificationLoadRoots(uint64_t title_id) const;

    FileSys::VirtualDir GetSystemNANDContentDirectory() const;

    void CreateFactories(FileSys::VfsFilesystem & vfs, bool overwrite = true);

    // IFileSystemController
    IFileSysRegisteredCache & GetSystemNANDContents() const override;
    ISaveDataController * OpenSaveDataController() const override;
    uint64_t GetFreeSpaceSize(StorageId id) const override;
    uint64_t GetTotalSpaceSize(StorageId id) const override;
    bool OpenProcess(uint64_t * programId, ISaveDataFactory ** saveDataFactory, IRomFsController ** romFsController, uint64_t processId) override;
    bool OpenSDMC(IVirtualDirectory ** out_sdmc) const override;

private:
    std::shared_ptr<FileSys::SaveDataFactory> CreateSaveDataFactory(FileSys::ProgramId program_id) const;

    struct Registration {
        FileSys::ProgramId program_id;
        std::shared_ptr<FileSys::RomFSFactory> romfs_factory;
        std::shared_ptr<FileSys::SaveDataFactory> save_data_factory;
    };
    std::mutex registration_lock;
    std::map<FileSys::ProcessId, Registration> registrations;

    std::unique_ptr<FileSys::SDMCFactory> sdmc_factory;
    std::unique_ptr<FileSys::BISFactory> bis_factory;

    Systemloader & loader;
};
