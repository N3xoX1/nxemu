// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <memory>
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "core/hle/result.h"

class Systemloader;

class SaveDataController
{
public:
    explicit SaveDataController(Systemloader & loader_, std::shared_ptr<FileSys::SaveDataFactory> factory_);
    ~SaveDataController();

    bool CreateSaveData(FileSys::VirtualDir* out_save_data, SaveDataSpaceId space, const SaveDataAttribute& attribute);
    Result OpenSaveData(FileSys::VirtualDir* out_save_data, SaveDataSpaceId space, const SaveDataAttribute& attribute);
    Result OpenSaveDataSpace(FileSys::VirtualDir* out_save_data_space, SaveDataSpaceId space);

    SaveDataSize ReadSaveDataSize(SaveDataType type, uint64_t title_id, u128 user_id);
    void WriteSaveDataSize(SaveDataType type, uint64_t title_id, u128 user_id, SaveDataSize new_value);
    void SetAutoCreate(bool state);

private:
    Systemloader & loader;
    const std::shared_ptr<FileSys::SaveDataFactory> factory;
};

class SaveDataControllerPtr :
    public ISaveDataController
{
public:
    SaveDataControllerPtr();
    SaveDataControllerPtr(std::shared_ptr<SaveDataController> saveDataController);
    SaveDataControllerPtr(SaveDataControllerPtr&& other) noexcept;
    ~SaveDataControllerPtr();

    SaveDataControllerPtr & operator=(SaveDataControllerPtr && other) noexcept;

    SaveDataController * operator->() const;
    SaveDataController & operator*() const;
    operator bool() const;

    // ISaveDataController
    bool CreateSaveData(IVirtualDirectory ** out_save_data, SaveDataSpaceId space, const SaveDataAttribute & attribute) override;
    SaveDataSize ReadSaveDataSize(SaveDataType type, uint64_t title_id, uint64_t user_id_high, uint64_t user_id_low) override;
    void WriteSaveDataSize(SaveDataType type, uint64_t title_id, uint64_t user_id_high, uint64_t user_id_low, SaveDataSize new_value) override;
    void Release() override;

private:
    SaveDataControllerPtr(const SaveDataControllerPtr &) = delete;
    SaveDataControllerPtr & operator=(const SaveDataControllerPtr &) = delete;

    std::shared_ptr<SaveDataController> m_saveDataController;
};
