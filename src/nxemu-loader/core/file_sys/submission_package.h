// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <memory>
#include <set>
#include <vector>
#include "yuzu_common/common_types.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/vfs/vfs.h"


namespace FileSys {

class PartitionFilesystem;

class NSP : public ReadOnlyVfsDirectory {
public:
    explicit NSP(VirtualFile file_, uint64_t title_id = 0, std::size_t program_index_ = 0,
                 uint64_t content_filter_base_id = 0);
    ~NSP() override;

    LoaderResultStatus GetStatus() const;
    LoaderResultStatus GetProgramStatus() const;
    // Should only be used when one title id can be assured.
    uint64_t GetProgramTitleID() const;
    uint64_t GetExtractedTitleID() const;
    std::vector<uint64_t> GetProgramTitleIDs() const;

    bool IsExtractedType() const;

    // Common (Can be safely called on both types)
    VirtualFile GetRomFS() const;
    VirtualDir GetExeFS() const;

    // Type 0 Only (Collection of NCAs + Certificate + Ticket + Meta XML)
    std::vector<std::shared_ptr<NCA>> GetNCAsCollapsed() const;
    std::multimap<uint64_t, std::shared_ptr<NCA>> GetNCAsByTitleID() const;
    std::map<uint64_t, std::map<std::pair<LoaderTitleType, LoaderContentRecordType>, std::shared_ptr<NCA>>> GetNCAs()
        const;
    std::shared_ptr<NCA> GetNCA(uint64_t title_id, LoaderContentRecordType type,
                                LoaderTitleType title_type = LoaderTitleType::Application) const;
    VirtualFile GetNCAFile(uint64_t title_id, LoaderContentRecordType type,
                           LoaderTitleType title_type = LoaderTitleType::Application) const;

    std::vector<VirtualFile> GetFiles() const override;

    std::vector<VirtualDir> GetSubdirectories() const override;

    std::string GetName() const override;

    VirtualDir GetParentDirectory() const override;

private:
    void InitializeExeFSAndRomFS(const std::vector<VirtualFile>& files);
    void ReadNCAs(const std::vector<VirtualFile>& files);

    VirtualFile file;

    const uint64_t expected_program_id;
    const std::size_t program_index;
    const uint64_t content_filter_base_id;

    bool extracted = false;
    LoaderResultStatus status;
    std::map<uint64_t, LoaderResultStatus> program_status;

    std::shared_ptr<PartitionFilesystem> pfs;
    // Map title id -> {map type -> NCA}
    std::map<uint64_t, std::map<std::pair<LoaderTitleType, LoaderContentRecordType>, std::shared_ptr<NCA>>> ncas;
    std::set<uint64_t> program_ids;
    std::vector<VirtualFile> ticket_files;

    VirtualFile romfs;
    VirtualDir exefs;
};
} // namespace FileSys
