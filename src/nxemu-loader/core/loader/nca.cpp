// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>

#include "system_loader.h"
#include "yuzu_common/hex_util.h"
#include "yuzu_common/scope_exit.h"
#include "yuzu_common/literals.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nca.h"

namespace Loader {

AppLoader_NCA::AppLoader_NCA(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), nca(std::make_unique<FileSys::NCA>(file)) {}

AppLoader_NCA::~AppLoader_NCA() = default;

LoaderFileType AppLoader_NCA::IdentifyType(const FileSys::VirtualFile & nca_file)
{
    const FileSys::NCA nca(nca_file);

    if (nca.GetStatus() == LoaderResultStatus::Success && nca.GetType() == FileSys::NCAContentType::Program)
    {
        return LoaderFileType::NCA;
    }

    return LoaderFileType::Error;
}

AppLoader_NCA::LoadResult AppLoader_NCA::Load(Systemloader & loader, ISystemModules & systemModules)
{
    if (is_loaded)
    {
        return {LoaderResultStatus::ErrorAlreadyLoaded, {}};
    }

    const auto result = nca->GetStatus();
    if (result != LoaderResultStatus::Success)
    {
        return {result, {}};
    }

    if (nca->GetType() != FileSys::NCAContentType::Program)
    {
        return {LoaderResultStatus::ErrorNCANotProgram, {}};
    }

    auto exefs = nca->GetExeFS();
    if (exefs == nullptr)
    {
        LOG_INFO(Loader, "No ExeFS found in NCA, looking for ExeFS from update");

        // This NCA may be a sparse base of an installed title.
        // Try to fetch the ExeFS from the installed update.
        const auto& installed = loader.GetContentProvider();
        const auto update_nca = installed.GetEntryNCA(FileSys::GetUpdateTitleID(nca->GetTitleId()), LoaderContentRecordType::Program);

        if (update_nca)
        {
            exefs = update_nca->GetExeFS();
        }

        if (exefs == nullptr)
        {
            return { LoaderResultStatus::ErrorNoExeFS, {}};
        }
    }

    directory_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs, true);

    const auto load_result = directory_loader->Load(loader, systemModules);
    if (load_result.first != LoaderResultStatus::Success)
    {
        return load_result;
    }

    FileSys::VirtualFile romFS;
    if (ReadRomFS(romFS) != LoaderResultStatus::Success)
    {
        LOG_WARNING(Service_FS, "Unable to read base RomFS");
    }

    loader.GetFileSystemController().RegisterProcess(load_result.second->process_id, nca->GetTitleId(),std::make_shared<FileSys::RomFSFactory>(romFS, IsRomFSUpdatable(), loader.GetContentProvider(), loader.GetFileSystemController()));
    is_loaded = true;
    return load_result;
}

LoaderResultStatus AppLoader_NCA::VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback)
{
    using namespace Common::Literals;

    constexpr size_t NcaFileNameWithHashLength = 36;
    constexpr size_t NcaFileNameHashLength = 32;
    constexpr size_t NcaSha256HashLength = 32;
    constexpr size_t NcaSha256HalfHashLength = NcaSha256HashLength / 2;

    // Get the file name.
    const auto name = file->GetName();

    // We won't try to verify meta NCAs.
    if (name.ends_with(".cnmt.nca"))
    {
        return LoaderResultStatus::Success;
    }

    // Check if we can verify this file. NCAs should be named after their hashes.
    if (!name.ends_with(".nca") || name.size() != NcaFileNameWithHashLength)
    {
        LOG_WARNING(Loader, "Unable to validate NCA with name {}", name);
        return LoaderResultStatus::ErrorIntegrityVerificationNotImplemented;
    }

    // Get the expected truncated hash of the NCA.
    const auto input_hash = Common::HexStringToVector(file->GetName().substr(0, NcaFileNameHashLength), false);

    // Declare buffer to read into.
    std::vector<u8> buffer(4_MiB);

    UNIMPLEMENTED();

    // File verified.
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_NCA::ReadRomFS(FileSys::VirtualFile& dir)
{
    if (nca == nullptr)
    {
        return LoaderResultStatus::ErrorNotInitialized;
    }

    if (nca->RomFS() == nullptr || nca->RomFS()->GetSize() == 0)
    {
        return LoaderResultStatus::ErrorNoRomFS;
    }

    dir = nca->RomFS();
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_NCA::ReadProgramId(uint64_t& out_program_id)
{
    if (nca == nullptr || nca->GetStatus() != LoaderResultStatus::Success)
    {
        return LoaderResultStatus::ErrorNotInitialized;
    }

    out_program_id = nca->GetTitleId();
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_NCA::ReadBanner(std::vector<u8>& buffer)
{
    if (nca == nullptr || nca->GetStatus() != LoaderResultStatus::Success)
    {
        return LoaderResultStatus::ErrorNotInitialized;
    }

    const auto logo = nca->GetLogoPartition();
    if (logo == nullptr)
    {
        return LoaderResultStatus::ErrorNoIcon;
    }

    buffer = logo->GetFile("StartupMovie.gif")->ReadAllBytes();
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_NCA::ReadLogo(std::vector<u8>& buffer)
{
    if (nca == nullptr || nca->GetStatus() != LoaderResultStatus::Success)
    {
        return LoaderResultStatus::ErrorNotInitialized;
    }

    const auto logo = nca->GetLogoPartition();
    if (logo == nullptr)
    {
        return LoaderResultStatus::ErrorNoIcon;
    }

    buffer = logo->GetFile("NintendoLogo.png")->ReadAllBytes();
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_NCA::ReadNSOModules(Modules& modules)
{
    if (directory_loader == nullptr)
    {
        return LoaderResultStatus::ErrorNotInitialized;
    }
    return directory_loader->ReadNSOModules(modules);
}

} // namespace Loader
