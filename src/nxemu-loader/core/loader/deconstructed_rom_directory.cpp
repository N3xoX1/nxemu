// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/loader/deconstructed_rom_directory.h"
#include "core/core.h"
#include "core/file_sys/content_archive.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs_factory.h"
#include "core/loader/nso.h"
#include "loader_settings_identifiers.h"
#include "system_loader.h"
#include "yuzu_common/interface_pointer.h"
#include "yuzu_common/interface_pointer_def.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/settings.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-module-spec/cpu.h>

using IPatchCollectionPtr = InterfacePtr<IPatchCollection>;
template class InterfacePtr<IPatchCollection>;

extern IModuleSettings * g_settings;

namespace Loader {

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(FileSys::VirtualFile file_, bool override_update_) : 
    AppLoader(std::move(file_)), 
    override_update(override_update_), 
    is_hbl(false) 
{
    const auto file_dir = file->GetContainingDirectory();

    // Title ID
    const auto npdm = file_dir->GetFile("main.npdm");
    if (npdm != nullptr)
    {
        const auto res = metadata.Load(npdm);
        if (res == LoaderResultStatus::Success)
        {
            title_id = metadata.GetTitleID();        
        }
    }

    // Icon
    FileSys::VirtualFile icon_file = nullptr;
    for (const auto& language : FileSys::LANGUAGE_NAMES)
    {
        icon_file = file_dir->GetFile("icon_" + std::string(language) + ".dat");
        if (icon_file != nullptr)
        {
            icon_data = icon_file->ReadAllBytes();
            break;
        }
    }

    if (icon_data.empty())
    {
        // Any png, jpeg, or bmp file
        const auto& files = file_dir->GetFiles();
        const auto icon_iter = std::find_if(files.begin(), files.end(), [](const FileSys::VirtualFile& f)
            {
                return f->GetExtension() == "png" || f->GetExtension() == "jpg" || f->GetExtension() == "bmp" || f->GetExtension() == "jpeg";
            });
        if (icon_iter != files.end())
        {        
            icon_data = (*icon_iter)->ReadAllBytes();
        }
    }

    // Metadata
    FileSys::VirtualFile nacp_file = file_dir->GetFile("control.nacp");
    if (nacp_file == nullptr)
    {
        const auto& files = file_dir->GetFiles();
        const auto nacp_iter = std::find_if(files.begin(), files.end(), [](const FileSys::VirtualFile& f) { return f->GetExtension() == "nacp"; });
        if (nacp_iter != files.end())
        {
            nacp_file = *nacp_iter;        
        }
    }

    if (nacp_file != nullptr)
    {
        FileSys::NACP nacp(nacp_file);
        name = nacp.GetApplicationName();
    }
}

AppLoader_DeconstructedRomDirectory::AppLoader_DeconstructedRomDirectory(FileSys::VirtualDir directory, bool override_update_, bool is_hbl_) :
    AppLoader(directory->GetFile("main")),
    dir(std::move(directory)),
    override_update(override_update_), 
    is_hbl(is_hbl_)
{
}

LoaderFileType AppLoader_DeconstructedRomDirectory::IdentifyType(const FileSys::VirtualFile & dir_file)
{
    if (FileSys::IsDirectoryExeFS(dir_file->GetContainingDirectory()))
    {
        return LoaderFileType::DeconstructedRomDirectory;
    }
    return LoaderFileType::Error;
}

AppLoader_DeconstructedRomDirectory::LoadResult AppLoader_DeconstructedRomDirectory::Load(Systemloader & loader, ISystemModules & systemModules)
{
    if (is_loaded)
    {
        return { LoaderResultStatus::ErrorAlreadyLoaded, {}};
    }

    if (dir == nullptr)
    {
        if (file == nullptr)
        {
            return { LoaderResultStatus::ErrorNullFile, {}};
        }

        dir = file->GetContainingDirectory();
    }

    // Read meta to determine title ID
    FileSys::VirtualFile npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr)
    {
        return { LoaderResultStatus::ErrorMissingNPDM, {}};
    }

    const LoaderResultStatus result = metadata.Load(npdm);
    if (result != LoaderResultStatus::Success)
    {
        return {result, {}};
    }

    if (override_update)
    {
        const FileSys::PatchManager patch_manager(metadata.GetTitleID(), loader.GetFileSystemController(), loader.GetContentProvider());
        dir = patch_manager.PatchExeFS(dir);
    }

    // Reread in case PatchExeFS affected the main.npdm
    npdm = dir->GetFile("main.npdm");
    if (npdm == nullptr)
    {
        return {LoaderResultStatus::ErrorMissingNPDM, {}};
    }

    const LoaderResultStatus result2 = metadata.Reload(npdm);
    if (result2 != LoaderResultStatus::Success)
    {
        return {result2, {}};
    }
    metadata.Print();

    const bool is_39bit = metadata.GetAddressSpaceType() == ProgramAddressSpaceType::Is39Bit;
    const bool is_application = metadata.GetPoolPartition() == PoolPartition::Application;
    g_settings->SetBool(NXLoaderSetting::Has39BitAddressSpace, is_39bit);

    IPatchCollectionPtr patch_ctx(systemModules.Cpu().CreatePatchCollection(is_application));
    const std::array static_modules = {"rtld", "main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"};
    std::size_t code_size{};

    // Use the NSO module loader to figure out the code layout
    for (size_t i = 0; i < static_modules.size(); i++)
    {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file)
        {
            continue;
        }

        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const int32_t patch_index = patch_ctx ? patch_ctx->GetLastIndex() : -1;
        const std::optional<VAddr> tentative_next_load_addr = AppLoader_NSO::LoadModule(loader, systemModules, *module_file, code_size, should_pass_arguments, false, {}, patch_ctx.Get(), patch_index);
        if (!tentative_next_load_addr)
        {
            return {LoaderResultStatus::ErrorLoadingNSO, {}};
        }

        if (patch_ctx)
        {
            patch_ctx->SaveIndex((uint32_t)i);
        }
        code_size = *tentative_next_load_addr;
    }

    // Enable direct memory mapping in case of NCE.
    const uint64_t fastmem_base = [&]() -> size_t
    {
        if (is_application && g_settings->GetBool(NXCpuSetting::NceEnabled))
        {
            UNIMPLEMENTED();
            return 0;
        }
        return 0;
    }();

    // Add patch size to the total module size
    code_size += patch_ctx ? patch_ctx->GetTotalPatchSize() : 0;

    // Setup the process code layout
    IOperatingSystem & operatingSystem = systemModules.OperatingSystem();
    uint64_t base_address = 0;
    uint64_t processID = 0;
    if (!operatingSystem.CreateApplicationProcess(code_size, metadata, base_address, processID, is_hbl))
    {
        return {LoaderResultStatus::ErrorUnableToParseKernelMetadata, {}};
    }
    loader.SetProcessID(processID);
    loader.SetTitleID(metadata.GetTitleID());

    // Load NSO modules
    modules.clear();
    VAddr next_load_addr{base_address};
    const FileSys::PatchManager pm{metadata.GetTitleID(), loader.GetFileSystemController(), loader.GetContentProvider()};
    for (size_t i = 0; i < static_modules.size(); i++)
    {
        const auto& module = static_modules[i];
        const FileSys::VirtualFile module_file{dir->GetFile(module)};
        if (!module_file)
        {
            continue;
        }

        const VAddr load_addr{next_load_addr};
        const bool should_pass_arguments = std::strcmp(module, "rtld") == 0;
        const int32_t patch_index = patch_ctx ? patch_ctx->GetIndex((uint32_t)i) : -1;
        const auto tentative_next_load_addr = AppLoader_NSO::LoadModule(loader, systemModules, *module_file, load_addr, should_pass_arguments, true, pm, patch_ctx.Get(), patch_index);
        if (!tentative_next_load_addr)
        {
            return {LoaderResultStatus::ErrorLoadingNSO, {}};
        }

        next_load_addr = *tentative_next_load_addr;
        modules.insert_or_assign(load_addr, module);
        LOG_DEBUG(Loader, "loaded module {} @ {:#X}", module, load_addr);
    }

    is_loaded = true;
    return {LoaderResultStatus::Success, LoadParameters{metadata.GetMainThreadPriority(), metadata.GetMainThreadStackSize(), base_address, processID}};
}

LoaderResultStatus AppLoader_DeconstructedRomDirectory::ReadRomFS(FileSys::VirtualFile& out_dir)
{
    if (romfs == nullptr)
    {
        return LoaderResultStatus::ErrorNoRomFS;
    }

    out_dir = romfs;
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_DeconstructedRomDirectory::ReadIcon(uint8_t * buffer, uint32_t * bufferSize)
{
    if (icon_data.empty()) 
    {
        return LoaderResultStatus::ErrorNoIcon;
    }

    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    if (buffer != nullptr && * bufferSize < (uint32_t)icon_data.size())
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = (uint32_t)icon_data.size();
    if (buffer == nullptr)
    {
        return LoaderResultStatus::Success;
    }
    std::memcpy(buffer, icon_data.data(), icon_data.size());
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_DeconstructedRomDirectory::ReadProgramId(uint64_t& out_program_id)
{
    out_program_id = title_id;
    return LoaderResultStatus::Success;
}

LoaderResultStatus AppLoader_DeconstructedRomDirectory::ReadTitle(char * buffer, uint32_t * bufferSize)
{
    if (name.empty()) 
    {
        return LoaderResultStatus::ErrorNoControl;
    }
    if (bufferSize == nullptr)
    {
        return LoaderResultStatus::ErrorNotImplemented;
    }
    if (buffer != nullptr && *bufferSize < (uint32_t)name.size())
    {
        return LoaderResultStatus::ErrorBufferTooSmall;
    }
    *bufferSize = (uint32_t)name.size();
    if (buffer == nullptr)
    {
        return LoaderResultStatus::Success;
    }
    std::memcpy(buffer, name.data(), name.size());
    return LoaderResultStatus::Success;
}

bool AppLoader_DeconstructedRomDirectory::IsRomFSUpdatable() const
{
    return false;
}

LoaderResultStatus AppLoader_DeconstructedRomDirectory::ReadNSOModules(Modules& out_modules)
{
    if (!is_loaded)
    {
        return LoaderResultStatus::ErrorNotInitialized;
    }

    out_modules = this->modules;
    return LoaderResultStatus::Success;
}

} // namespace Loader
