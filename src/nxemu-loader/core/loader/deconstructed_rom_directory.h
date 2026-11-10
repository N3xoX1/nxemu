// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include "yuzu_common/common_types.h"
#include "core/file_sys/program_metadata.h"
#include "core/loader/loader.h"

namespace Loader {

/**
 * This class loads a "deconstructed ROM directory", which are the typical format we see for Switch
 * game dumps. The path should be a "main" NSO, which must be in a directory that contains the other
 * standard ExeFS NSOs (e.g. rtld, sdk, etc.). It will automatically find and load these.
 * Furthermore, it will look for the first .romfs file (optionally) and use this for the RomFS.
 */
class AppLoader_DeconstructedRomDirectory final : public AppLoader {
public:
    explicit AppLoader_DeconstructedRomDirectory(FileSys::VirtualFile main_file,
                                                 bool override_update_ = false);

    // Overload to accept exefs directory. Must contain 'main' and 'main.npdm'
    explicit AppLoader_DeconstructedRomDirectory(FileSys::VirtualDir directory,
                                                 bool override_update_ = false,
                                                 bool is_hbl_ = false);

    /**
     * Identifies whether or not the given file is a deconstructed ROM directory.
     *
     * @param dir_file The file to verify.
     *
     * @return FileType::DeconstructedRomDirectory, or FileType::Error
     *         if the file is not a deconstructed ROM directory.
     */
    static LoaderFileType IdentifyType(const FileSys::VirtualFile & dir_file);

    LoaderFileType GetFileType() const override
    {
        return IdentifyType(file);
    }

    LoadResult Load(Systemloader & loader, ISystemModules & systemModules) override;

    LoaderResultStatus ReadRomFS(FileSys::VirtualFile& out_dir) override;
    LoaderResultStatus ReadControlData(FileSys::NACP& out_control) override;
    LoaderResultStatus ReadIcon(uint8_t * buffer, uint32_t * bufferSize) override;
    LoaderResultStatus ReadProgramId(uint64_t& out_program_id) override;
    LoaderResultStatus ReadTitle(char * buffer, uint32_t * bufferSize) override;
    bool IsRomFSUpdatable() const override;

    LoaderResultStatus ReadNSOModules(Modules& out_modules) override;

private:
    const FileSys::VirtualFile& LooseRomFS();

    FileSys::ProgramMetadata metadata;
    FileSys::VirtualFile romfs;
    FileSys::VirtualDir romfs_exefs_dir;
    std::once_flag romfs_once;
    FileSys::VirtualDir dir;

    std::vector<u8> icon_data;
    std::optional<FileSys::NACP> control;
    std::string name;
    uint64_t title_id{};
    bool override_update;
    bool is_hbl;

    Modules modules;
};

} // namespace Loader
