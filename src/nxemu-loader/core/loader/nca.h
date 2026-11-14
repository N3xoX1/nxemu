// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "yuzu_common/common_types.h"
#include "core/loader/loader.h"

namespace Core {
class System;
}

namespace FileSys {
class NCA;
}

namespace Loader {

class AppLoader_DeconstructedRomDirectory;

/// Loads an NCA file
class AppLoader_NCA final : public AppLoader {
public:
    explicit AppLoader_NCA(FileSys::VirtualFile file_);
    ~AppLoader_NCA() override;

    /**
     * Identifies whether or not the given file is an NCA file.
     *
     * @param nca_file The file to identify.
     *
     * @return FileType::NCA, or FileType::Error if the file is not an NCA file.
     */
    static LoaderFileType IdentifyType(const FileSys::VirtualFile & nca_file);

    LoaderFileType GetFileType() const override
    {
        return IdentifyType(file);
    }

    LoadResult Load(Systemloader & loader, ISystemModules & systemModules) override;

    void SetPackedUpdate(FileSys::VirtualFile file, u32 version = 0);

    LoaderResultStatus VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback) override;

    LoaderResultStatus ReadRomFS(FileSys::VirtualFile& dir) override;
    LoaderResultStatus ReadProgramId(uint64_t& out_program_id) override;

    LoaderResultStatus ReadBanner(std::vector<u8>& buffer) override;
    LoaderResultStatus ReadLogo(std::vector<u8>& buffer) override;

    LoaderResultStatus ReadNSOModules(Modules& modules) override;

private:
    // The RomFS factory can need this base NCA to resolve a BKTR update after
    // the loader has returned.
    std::shared_ptr<FileSys::NCA> nca;
    FileSys::VirtualFile packed_update;
    u32 packed_update_version{};
    std::unique_ptr<AppLoader_DeconstructedRomDirectory> directory_loader;
};

} // namespace Loader
