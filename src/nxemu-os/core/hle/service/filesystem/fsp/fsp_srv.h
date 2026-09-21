// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "core/hle/service/cmif_types.h"
#include "core/hle/service/filesystem/fsp/fsp_types.h"
#include "core/hle/service/service.h"
#include "core/file_sys/fs_save_data_types.h"
#include <nxemu-module-spec/system_loader.h>
#include <yuzu_common/fs/filesystem_interfaces.h>

namespace Core {
class Reporter;
}

namespace FileSys {
class ContentProvider;
class FileSystemBackend;
} // namespace FileSys

namespace Service::FileSystem {

class RomFsController;
class SaveDataController;

class IFileSystem;
class ISaveDataInfoReader;
class ISaveDataTransferProhibiter;
class IStorage;
class IMultiCommitManager;

enum class AccessLogVersion : u32 {
    V7_0_0 = 2,

    Latest = V7_0_0,
};

enum class AccessLogMode : u32 {
    None,
    Log,
    SdCard,
};

class FSP_SRV final : public ServiceFramework<FSP_SRV> {
public:
    explicit FSP_SRV(Core::System & system_);
    ~FSP_SRV() override;

private:
    Result SetCurrentProcess(ClientProcessId pid);
    Result OpenFileSystemWithPatch(OutInterface<IFileSystem> out_interface, FileSystemProxyType type, u64 open_program_id);
    Result OpenSdCardFileSystem(OutInterface<IFileSystem> out_interface);
    Result IsExFatSupported(Out<bool> out_is_supported);
    Result CreateSaveDataFileSystem(FileSys::SaveDataCreationInfo save_create_struct, SaveDataAttribute save_struct, u128 uid);
    Result CreateSaveDataFileSystemBySystemSaveDataId(SaveDataAttribute save_struct, FileSys::SaveDataCreationInfo save_create_struct);
    Result OpenSaveDataFileSystem(OutInterface<IFileSystem> out_interface, SaveDataSpaceId space_id, SaveDataAttribute attribute);
    Result OpenSaveDataFileSystemBySystemSaveDataId(OutInterface<IFileSystem> out_interface, SaveDataSpaceId space_id, SaveDataAttribute attribute);
    Result OpenReadOnlySaveDataFileSystem(OutInterface<IFileSystem> out_interface, SaveDataSpaceId space_id, SaveDataAttribute attribute);
    Result OpenSaveDataInfoReaderBySaveDataSpaceId(OutInterface<ISaveDataInfoReader> out_interface, SaveDataSpaceId space);
    Result OpenSaveDataInfoReaderOnlyCacheStorage(OutInterface<ISaveDataInfoReader> out_interface);
    Result FindSaveDataWithFilter(Out<s64> out_count, OutBuffer<BufferAttr_HipcMapAlias> out_buffer, SaveDataSpaceId space_id, FileSys::SaveDataFilter filter);
    Result WriteSaveDataFileSystemExtraData(InBuffer<BufferAttr_HipcMapAlias> buffer, SaveDataSpaceId space_id, u64 save_data_id);
    Result WriteSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(InBuffer<BufferAttr_HipcMapAlias> buffer, InBuffer<BufferAttr_HipcMapAlias> mask_buffer, SaveDataSpaceId space_id, SaveDataAttribute attribute);
    Result ReadSaveDataFileSystemExtraData(OutBuffer<BufferAttr_HipcMapAlias> out_buffer, u64 save_data_id);
    Result ReadSaveDataFileSystemExtraDataBySaveDataAttribute(OutBuffer<BufferAttr_HipcMapAlias> out_buffer, SaveDataSpaceId space_id, SaveDataAttribute attribute);
    Result ReadSaveDataFileSystemExtraDataBySaveDataSpaceId(OutBuffer<BufferAttr_HipcMapAlias> out_buffer, SaveDataSpaceId space_id, u64 save_data_id);
    Result ReadSaveDataFileSystemExtraDataWithMaskBySaveDataAttribute(SaveDataSpaceId space_id, SaveDataAttribute attribute, InBuffer<BufferAttr_HipcMapAlias> mask_buffer, OutBuffer<BufferAttr_HipcMapAlias> out_buffer);
    Result OpenSaveDataTransferProhibiter(OutInterface<ISaveDataTransferProhibiter> out_prohibiter, u64 id);
    Result OpenDataStorageByCurrentProcess(OutInterface<IStorage> out_interface);
    Result OpenDataStorageByDataId(OutInterface<IStorage> out_interface, StorageId storage_id, u32 unknown, u64 title_id);
    Result OpenPatchDataStorageByCurrentProcess(OutInterface<IStorage> out_interface, StorageId storage_id, u64 title_id);
    Result OpenDataStorageWithProgramIndex(OutInterface<IStorage> out_interface, u8 program_index);
    Result DisableAutoSaveDataCreation();
    Result SetGlobalAccessLogMode(AccessLogMode access_log_mode_);
    Result GetGlobalAccessLogMode(Out<AccessLogMode> out_access_log_mode);
    Result OutputAccessLogToSdCard(InBuffer<BufferAttr_HipcMapAlias> log_message_buffer);
    Result FlushAccessLogOnSdCard();
    Result GetProgramIndexForAccessLog(Out<AccessLogVersion> out_access_log_version, Out<u32> out_access_log_program_index);
    Result OpenMultiCommitManager(OutInterface<IMultiCommitManager> out_interface);
    Result ExtendSaveDataFileSystem(SaveDataSpaceId space_id, u64 save_data_id, s64 available_size, s64 journal_size);
    Result GetCacheStorageSize(s32 index, Out<s64> out_data_size, Out<s64> out_journal_size);

    IFileSystemController & fsc;
    IVirtualFilePtr romfs;
    u64 current_process_id = 0;
    u32 access_log_program_index = 0;
    AccessLogMode access_log_mode = AccessLogMode::None;
    u64 program_id = 0;
    SaveDataFactoryPtr save_data_controller;
    RomFsControllerPtr romfs_controller;
};

} // namespace Service::FileSystem
