// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include "yuzu_common/common_funcs.h"
#include "yuzu_common/common_types.h"
#include "core/file_sys/fs_save_data_types.h"
#include "core/file_sys/vfs/vfs.h"

namespace FileSys {

constexpr const char* GetSaveDataSizeFileName() 
{
    return ".nxemu_save_size";
}

using ProgramId = uint64_t;

/// File system interface to the SaveData archive
class SaveDataFactory 
{
public:
    explicit SaveDataFactory(ProgramId program_id_, VirtualDir save_directory_);
    ~SaveDataFactory();

    VirtualDir Create(SaveDataSpaceId space, const SaveDataAttribute& meta) const;
    VirtualDir Open(SaveDataSpaceId space, const SaveDataAttribute& meta) const;

    VirtualDir GetSaveDataSpaceDirectory(SaveDataSpaceId space) const;

    static std::string GetSaveDataSpaceIdPath(SaveDataSpaceId space);
    static std::string GetFullPath(ProgramId program_id, VirtualDir dir, SaveDataSpaceId space, SaveDataType type, uint64_t title_id, u128 user_id, uint64_t save_id);
    static std::string GetUserGameSaveDataRoot(u128 user_id, bool future);

    SaveDataSize ReadSaveDataSize(SaveDataType type, uint64_t title_id, u128 user_id) const;
    void WriteSaveDataSize(SaveDataType type, uint64_t title_id, u128 user_id, SaveDataSize new_value) const;

    void SetAutoCreate(bool state);

private:
    ProgramId program_id;
    VirtualDir dir;
    bool auto_create;
};

} // namespace FileSys

class SaveDataFactoryImpl :
    public ISaveDataFactory
{
public:
    SaveDataFactoryImpl();
    SaveDataFactoryImpl(std::shared_ptr<FileSys::SaveDataFactory> saveDataFactory);
    SaveDataFactoryImpl(SaveDataFactoryImpl && other) noexcept;
    ~SaveDataFactoryImpl();

    SaveDataFactoryImpl & operator=(SaveDataFactoryImpl && other) noexcept;

    FileSys::SaveDataFactory * operator->() const;
    FileSys::SaveDataFactory & operator*() const;
    operator bool() const;

    // ISaveDataFactory
    bool OpenSaveData(IVirtualDirectory ** out_save_data, SaveDataSpaceId space, const SaveDataAttribute & attribute) override;
    void SetAutoCreate(bool state) override;
    void Release() override;

private:
    SaveDataFactoryImpl(const SaveDataFactoryImpl &) = delete;
    SaveDataFactoryImpl & operator=(const SaveDataFactoryImpl &) = delete;

    std::shared_ptr<FileSys::SaveDataFactory> m_saveDataFactory;
};
