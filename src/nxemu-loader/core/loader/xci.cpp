// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vector>

#include "yuzu_common/common_types.h"
#include "yuzu_common/yuzu_assert.h"
#include "core/core.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/filesystem.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/submission_package.h"
#include "core/loader/nca.h"
#include "core/loader/xci.h"
#include "system_loader.h"

namespace Loader {

AppLoader_XCI::AppLoader_XCI(FileSys::VirtualFile file_, const FileSystemController& fsc, const FileSys::ContentProvider& content_provider, uint64_t program_id, std::size_t program_index) : 
    AppLoader(file_),
    xci(std::make_unique<FileSys::XCI>(file_, program_id, program_index)),
    nca_loader(std::make_unique<AppLoader_NCA>(xci->GetProgramNCAFile()))
{
    if (xci->GetStatus() != LoaderResultStatus::Success)
    {
        return;
    }

    const auto control_nca = xci->GetNCAByType(FileSys::NCAContentType::Control);
    if (control_nca == nullptr || control_nca->GetStatus() != LoaderResultStatus::Success)
    {
        return;
    }

    std::tie(nacp_file, icon_file) = [this, &content_provider, &control_nca, &fsc]
    {
        const FileSys::PatchManager pm{xci->GetProgramTitleID(), fsc, content_provider};
        return pm.ParseControlNCA(*control_nca);
    }();
}

AppLoader_XCI::~AppLoader_XCI() = default;

LoaderFileType AppLoader_XCI::IdentifyType(const FileSys::VirtualFile & xci_file)
{
    const FileSys::XCI xci(xci_file);
    if (xci.GetStatus() == LoaderResultStatus::Success && xci.GetNCAByType(FileSys::NCAContentType::Program) != nullptr && AppLoader_NCA::IdentifyType(xci.GetNCAFileByType(FileSys::NCAContentType::Program)) == LoaderFileType::NCA)
    {
        return LoaderFileType::XCI;
    }
    return LoaderFileType::Error;
}

AppLoader_XCI::LoadResult AppLoader_XCI::Load(Systemloader & loader, ISystemModules & systemModules)
{
    if (is_loaded)
    {
        return {LoaderResultStatus::ErrorAlreadyLoaded, {}};
    }

    if (xci->GetStatus() != LoaderResultStatus::Success)
    {
        return {xci->GetStatus(), {}};
    }

    if (xci->GetProgramNCAStatus() != LoaderResultStatus::Success)
    {
        return {xci->GetProgramNCAStatus(), {}};
    }

    const auto result = nca_loader->Load(loader, systemModules);
    if (result.first != LoaderResultStatus::Success)
    {
        return result;
    }

    FileSys::VirtualFile update_raw;
    if (ReadUpdateRaw(update_raw) == LoaderResultStatus::Success && update_raw != nullptr)
    {
        loader.GetFileSystemController().SetPackedUpdate(result.second->process_id, std::move(update_raw));
    }
    is_loaded = true;
    return result;
}

LoaderResultStatus AppLoader_XCI::VerifyIntegrity(std::function<bool(size_t, size_t)> progress_callback)
{
    // Verify secure partition, as it is the only thing we can process.
    auto secure_partition = xci->GetSecurePartitionNSP();

    // Get list of all NCAs.
    UNIMPLEMENTED();
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_XCI::ReadRomFS(FileSys::VirtualFile& out_file)
{
    return nca_loader->ReadRomFS(out_file);
}

LoaderResultStatus AppLoader_XCI::ReadUpdateRaw(FileSys::VirtualFile& out_file)
{
    uint64_t program_id{};
    nca_loader->ReadProgramId(program_id);
    if (program_id == 0)
    {
        return LoaderResultStatus::ErrorXCIMissingProgramNCA;
    }

    const auto read = xci->GetSecurePartitionNSP()->GetNCAFile(FileSys::GetUpdateTitleID(program_id), LoaderContentRecordType::Program);
    if (read == nullptr)
    {
        return LoaderResultStatus::ErrorNoPackedUpdate;
    }

    const auto nca_test = std::make_shared<FileSys::NCA>(read);
    if (nca_test->GetStatus() != LoaderResultStatus::ErrorMissingBKTRBaseRomFS)
    {
        return nca_test->GetStatus();
    }

    out_file = read;
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_XCI::ReadProgramId(uint64_t& out_program_id)
{
    return nca_loader->ReadProgramId(out_program_id);
}

LoaderResultStatus AppLoader_XCI::ReadProgramIds(uint64_t * buffer, uint32_t * count)
{
    if (count == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    std::vector<uint64_t> program_ids = xci->GetProgramTitleIDs();
    const uint32_t required_count = (uint32_t)program_ids.size();

    if (buffer != nullptr && *count < required_count)
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *count = required_count;
    if (buffer == nullptr)
    {
        return LoaderResultStatus::Success;
    }
    std::memcpy(buffer, program_ids.data(), program_ids.size() * sizeof(uint64_t));
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_XCI::ReadIcon(uint8_t * buffer, uint32_t * bufferSize)
{
    if (icon_file == nullptr)
    {
        return LoaderResultStatus::ErrorNoIcon;
    }

    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    std::vector<u8> icon = icon_file->ReadAllBytes();
    if (buffer != nullptr && *bufferSize < (uint32_t)icon.size())
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = (uint32_t)icon.size();
    if (buffer == nullptr)
    {
        return LoaderResultStatus::Success;
    }
    std::memcpy(buffer, icon.data(), icon.size());
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_XCI::ReadTitle(char * buffer, uint32_t * bufferSize)
{
    if (nacp_file == nullptr)
    {
        return LoaderResultStatus::ErrorNoControl;
    }
    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    std::string title = nacp_file->GetApplicationName();
    if (buffer != nullptr && *bufferSize < (uint32_t)title.size())
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = (uint32_t)title.size();
    if (buffer == nullptr)
    {
        return LoaderResultStatus::Success;
    }
    std::memcpy(buffer, title.data(), title.size());
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_XCI::ReadControlData(FileSys::NACP& control)
{
    if (nacp_file == nullptr)
    {
        return LoaderResultStatus::ErrorNoControl;
    }

    control = *nacp_file;
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_XCI::ReadManualRomFS(FileSys::VirtualFile& out_file)
{
    UNIMPLEMENTED();
    return LoaderResultStatus::ErrorNoRomFS;
}

LoaderResultStatus AppLoader_XCI::ReadBanner(std::vector<u8>& buffer)
{
    return nca_loader->ReadBanner(buffer);
}

LoaderResultStatus AppLoader_XCI::ReadLogo(std::vector<u8>& buffer)
{
    return nca_loader->ReadLogo(buffer);
}

LoaderResultStatus AppLoader_XCI::ReadNSOModules(Modules& modules)
{
    return nca_loader->ReadNSOModules(modules);
}

} // namespace Loader
