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
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/nca.h"

namespace Loader {

AppLoader_NCA::AppLoader_NCA(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), nca(std::make_shared<FileSys::NCA>(file)) {}

AppLoader_NCA::~AppLoader_NCA() = default;

void AppLoader_NCA::SetPackedUpdate(FileSys::VirtualFile update, u32 version)
{
    packed_update = std::move(update);
    packed_update_version = version;
}

LoaderFileType AppLoader_NCA::IdentifyType(const FileSys::VirtualFile & nca_file)
{
    const FileSys::NCA nca(nca_file);

    if ((nca.GetStatus() == LoaderResultStatus::Success ||
         (nca.HasRomFSSectionReadFailure() && nca.GetExeFS() == nullptr)) &&
        nca.GetType() == FileSys::NCAContentType::Program)
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
    // A sparse base can have a RomFS section that is incomplete until its
    // enabled BKTR update is applied. Keep the program candidate loadable so
    // the update's standalone ExeFS can supply the executable below.
    const bool may_use_enabled_update_exefs =
        nca->HasRomFSSectionReadFailure() &&
        nca->GetType() == FileSys::NCAContentType::Program && nca->GetExeFS() == nullptr;
    if (result != LoaderResultStatus::Success && !may_use_enabled_update_exefs)
    {
        return {result, {}};
    }

    if (nca->GetType() != FileSys::NCAContentType::Program)
    {
        return {LoaderResultStatus::ErrorNCANotProgram, {}};
    }

    const FileSys::PatchManager patch_manager{
        nca->GetTitleId(), loader.GetFileSystemController(), loader.GetContentProvider()};
    auto exefs = patch_manager.PatchExeFS(nca->GetExeFS(), true, packed_update, packed_update_version);
    if (!exefs || !FileSys::IsDirectoryExeFS(exefs))
    {
        return {LoaderResultStatus::ErrorNoExeFS, {}};
    }
    directory_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs, false);

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

    loader.GetFileSystemController().RegisterProcess(
        load_result.second->process_id, nca->GetTitleId(),
        std::make_shared<FileSys::RomFSFactory>(romFS, IsRomFSUpdatable(),
                                               loader.GetContentProvider(),
                                               loader.GetFileSystemController(), nullptr, nca));
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
    if (nca == nullptr || (nca->GetStatus() != LoaderResultStatus::Success &&
                           !nca->HasRomFSSectionReadFailure()))
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
