// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include "yuzu_common/concepts.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/logging/log.h"
#include "yuzu_common/string_util.h"
#include "yuzu_common/yuzu_assert.h"
#include "loader.h"
#include "core/loader/nca.h"
#include "core/loader/nro.h"
#include "core/loader/nsp.h"
#include "core/loader/xci.h"
#include "system_loader.h"

namespace Loader {

namespace {

template <Common::DerivedFrom<AppLoader> T>
std::optional<LoaderFileType> IdentifyFileLoader(FileSys::VirtualFile file)
{
    const auto file_type = T::IdentifyType(file);
    if (file_type != LoaderFileType::Error)
    {
        return file_type;
    }
    return std::nullopt;
}

} // namespace

LoaderFileType IdentifyFile(FileSys::VirtualFile file)
{
    if (const auto nro_type = IdentifyFileLoader<AppLoader_NRO>(file))
    {
        return *nro_type;
    }
    else if (const auto nca_type = IdentifyFileLoader<AppLoader_NCA>(file))
    {
        return *nca_type;
    }
    else if (const auto xci_type = IdentifyFileLoader<AppLoader_XCI>(file))
    {
        return *xci_type;
    }
    else if (const auto nsp_type = IdentifyFileLoader<AppLoader_NSP>(file))
    {
        return *nsp_type;
    }
    else
    {
        UNIMPLEMENTED();
        return LoaderFileType::Unknown;
    }
}

LoaderFileType GuessFromFilename(const std::string & name)
{
    const std::string extension = Common::ToLower(std::string(Common::FS::GetExtensionFromFilename(name)));

    if (extension == "nro")
    {
        return LoaderFileType::NRO;
    }
    if (extension == "nca")
    {
        return LoaderFileType::NCA;
    }
    if (extension == "dxci")
    {
        return LoaderFileType::XCI;
    }
    if (extension == "dnsp")
    {
        return LoaderFileType::NSP;
    }
    UNIMPLEMENTED();
    return LoaderFileType::Unknown;
}

std::string GetFileTypeString(LoaderFileType type)
{
    switch (type) {
    case LoaderFileType::NRO:
        return "NRO";
    case LoaderFileType::XCI:
        return "XCI";
    case LoaderFileType::Error:
    case LoaderFileType::Unknown:
        break;
    }
    UNIMPLEMENTED();
    return "unknown";
}

AppLoader::AppLoader(FileSys::VirtualFile file_) : file(std::move(file_)) {}
AppLoader::~AppLoader() = default;

/**
 * Get a loader for a file with a specific type
 * @param system The system context to use.
 * @param file   The file to retrieve the loader for
 * @param type   The file type
 * @param program_index Specifies the index within the container of the program to launch.
 * @return std::shared_ptr<AppLoader> a pointer to a loader object;  nullptr for unsupported type
 */
static std::shared_ptr<AppLoader> GetFileLoader(Systemloader & loader, FileSys::VirtualFile file, LoaderFileType type, uint64_t program_id, std::size_t program_index)
{
    switch (type) {
    // NX NRO file format.
    case LoaderFileType::NRO:
        return std::make_shared<AppLoader_NRO>(std::move(file));
    // NX NCA (Nintendo Content Archive) file format.
    case LoaderFileType::NCA:
        return std::make_shared<AppLoader_NCA>(std::move(file));
    // NX XCI (nX Card Image) file format.
    case LoaderFileType::XCI:
        return std::make_shared<AppLoader_XCI>(std::move(file), loader.GetFileSystemController(), loader.GetContentProvider(), program_id, program_index);
    // NX NSP (Nintendo Submission Package) file format
    case LoaderFileType::NSP:
        return std::make_shared<AppLoader_NSP>(std::move(file), loader.GetFileSystemController(), loader.GetContentProvider(), program_id, program_index);
    }
    UNIMPLEMENTED();
    return nullptr;
}

std::shared_ptr<AppLoader> GetLoader(Systemloader & loader, FileSys::VirtualFile file, uint64_t program_id, std::size_t program_index)
{
    if (!file)
    {
        return nullptr;
    }

    LoaderFileType type = IdentifyFile(file);
    const LoaderFileType filename_type = GuessFromFilename(file->GetName());

    LOG_DEBUG(Loader, "Loading file {} as {}...", file->GetName(), GetFileTypeString(type));

    return GetFileLoader(loader, std::move(file), type, program_id, program_index);
}

} // namespace Loader
