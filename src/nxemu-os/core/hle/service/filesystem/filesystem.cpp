// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <utility>

#include "yuzu_common/yuzu_assert.h"
#include "yuzu_common/fs/fs.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/settings.h"
#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/fsp/fsp_ldr.h"
#include "core/hle/service/filesystem/fsp/fsp_pr.h"
#include "core/hle/service/filesystem/fsp/fsp_srv.h"
#include "core/hle/service/server_manager.h"

namespace Service::FileSystem {

static IVirtualDirectoryPtr GetDirectoryRelativeWrapped(const IVirtualDirectoryPtr & base, std::string_view dir_name_)
{
    std::string dir_name(Common::FS::SanitizePath(dir_name_));
    if (dir_name.empty() || dir_name == "." || dir_name == "/" || dir_name == "\\")
    {
        return IVirtualDirectoryPtr(base->Duplicate());
    }
    return IVirtualDirectoryPtr(base->GetDirectoryRelative(dir_name.c_str()));
}

VfsDirectoryServiceWrapper::VfsDirectoryServiceWrapper(IVirtualDirectoryPtr && backing_) : 
    backing(std::move(backing_)) 
{
}

VfsDirectoryServiceWrapper::~VfsDirectoryServiceWrapper() = default;

Result VfsDirectoryServiceWrapper::CreateFile(const std::string& path_, u64 size) const
{
    std::string path(Common::FS::SanitizePath(path_));
    
    IVirtualDirectoryPtr dir = GetDirectoryRelativeWrapped(backing, Common::FS::GetParentPath(path));
    if (!dir)
    {
        return FileSys::ResultPathNotFound;
    }

    FileSys::DirectoryEntryType entry_type{};
    if (GetEntryType(&entry_type, path) == ResultSuccess)
    {
        return FileSys::ResultPathAlreadyExists;
    }
    IVirtualFilePtr file(dir->CreateFile(Common::FS::GetFilename(path).data()));
    if (!file)
    {
        // TODO(DarkLordZach): Find a better error code for this
        return ResultUnknown;
    }
    if (!file->Resize(size))
    {
        // TODO(DarkLordZach): Find a better error code for this
        return ResultUnknown;
    }
    return ResultSuccess;
}

Result VfsDirectoryServiceWrapper::DeleteFile(const std::string & path_) const
{
    std::string path(Common::FS::SanitizePath(path_));
    if (path.empty())
    {
        // TODO(DarkLordZach): Why do games call this and what should it do? Works as is but...
        return ResultSuccess;
    }

    IVirtualDirectoryPtr dir = GetDirectoryRelativeWrapped(backing, Common::FS::GetParentPath(path));
    if (!dir)
    {
        return FileSys::ResultPathNotFound;
    }
    const std::string filename(Common::FS::GetFilename(path));
    IVirtualFilePtr file(dir->GetFile(filename.c_str()));
    if (!file)
    {
        return FileSys::ResultPathNotFound;
    }
    if (!dir->DeleteFile(filename.c_str()))
    {
        // TODO(DarkLordZach): Find a better error code for this
        return ResultUnknown;
    }

    return ResultSuccess;
}

Result VfsDirectoryServiceWrapper::CreateDirectory(const std::string& path_) const
{
    std::string path(Common::FS::SanitizePath(path_));

    // NOTE: This is inaccurate behavior. CreateDirectory is not recursive.
    // CreateDirectory should return PathNotFound if the parent directory does not exist.
    // This is here temporarily in order to have UMM "work" in the meantime.
    // TODO (Morph): Remove this when a hardware test verifies the correct behavior.
    const auto components = Common::FS::SplitPathComponents(path);
    std::string relative_path;
    for (const auto& component : components)
    {
        relative_path = Common::FS::SanitizePath(fmt::format("{}/{}", relative_path, component));
        IVirtualDirectoryPtr new_dir = backing->CreateSubdirectory(std::string(relative_path).c_str());
        if (!new_dir)
        {
            // TODO(DarkLordZach): Find a better error code for this
            return ResultUnknown;
        }
    }
    return ResultSuccess;
}

Result VfsDirectoryServiceWrapper::RenameFile(const std::string & src_path_, const std::string & dest_path_) const
{
    std::string src_path(Common::FS::SanitizePath(src_path_));
    std::string dest_path(Common::FS::SanitizePath(dest_path_));
    IVirtualFilePtr src(backing->GetFileRelative(src_path.c_str()));
    IVirtualFilePtr dst(backing->GetFileRelative(dest_path.c_str()));
    if (Common::FS::GetParentPath(src_path) == Common::FS::GetParentPath(dest_path))
    {
        // Use more-optimized vfs implementation rename.
        if (!src)
        {
            return FileSys::ResultPathNotFound;
        }

        if (dst && Common::FS::Exists(std::string(dst->GetFullPath())))
        {
            LOG_ERROR(Service_FS, "File at new_path={} already exists", dst->GetFullPath());
            return FileSys::ResultPathAlreadyExists;
        }

        const std::string dest_filename(Common::FS::GetFilename(dest_path));
        if (!src->Rename(dest_filename.c_str()))
        {
            // TODO(DarkLordZach): Find a better error code for this
            return ResultUnknown;
        }
        return ResultSuccess;
    }

    // Move by hand -- TODO(DarkLordZach): Optimize
    Result c_res = CreateFile(dest_path, src->GetSize());
    if (c_res != ResultSuccess)
    {
        return c_res;
    }

    IVirtualFilePtr dest(backing->GetFileRelative(dest_path.c_str()));
    ASSERT_MSG(dest, "Newly created file with success cannot be found.");

    std::vector<uint8_t> data = src.ReadAllBytes();
    ASSERT_MSG(dest->WriteBytes(data.data(), data.size(), 0) == src->GetSize(), "Could not write all of the bytes but everything else has succeeded.");

    IVirtualDirectoryPtr src_dir(src->GetContainingDirectory());
    const std::string src_filename(Common::FS::GetFilename(src_path));
    if (!src_dir || !src_dir->DeleteFile(src_filename.c_str()))
    {
        // TODO(DarkLordZach): Find a better error code for this
        return ResultUnknown;
    }
    return ResultSuccess;
}

Result VfsDirectoryServiceWrapper::OpenFile(IVirtualFile** out_file, const std::string& path_, VirtualFileOpenMode mode) const
{
    const std::string path(Common::FS::SanitizePath(path_));
    std::string_view npath = path;
    while (!npath.empty() && (npath[0] == '/' || npath[0] == '\\'))
    {
        npath.remove_prefix(1);
    }

    IVirtualFilePtr file(backing->GetFileRelative(npath.data()));
    if (!file)
    {
        return FileSys::ResultPathNotFound;
    }

    if (mode == VirtualFileOpenMode::AllowAppend) {
        UNIMPLEMENTED();
        //*out_file = std::make_shared<FileSys::OffsetVfsFile>(file, 0, file->GetSize());
    }
    else
    {
        *out_file = file.Detach();
    }
    return ResultSuccess;
}

Result VfsDirectoryServiceWrapper::OpenDirectory(IVirtualDirectory ** out_directory, const std::string & path_)
{
    std::string path(Common::FS::SanitizePath(path_));
    IVirtualDirectoryPtr dir = GetDirectoryRelativeWrapped(backing, path);
    if (!dir)
    {
        // TODO(DarkLordZach): Find a better error code for this
        return FileSys::ResultPathNotFound;
    }
    *out_directory = dir.Detach();
    return ResultSuccess;
}

Result VfsDirectoryServiceWrapper::GetEntryType(FileSys::DirectoryEntryType * out_entry_type, const std::string & path_) const
{
    std::string path(Common::FS::SanitizePath(path_));

    IVirtualDirectoryPtr dir = GetDirectoryRelativeWrapped(backing, Common::FS::GetParentPath(path));
    if (!dir)
    {
        return FileSys::ResultPathNotFound;
    }

    std::string_view filename = Common::FS::GetFilename(path);
    // TODO(Subv): Some games use the '/' path, find out what this means.
    if (filename.empty()) 
    {
        *out_entry_type = FileSys::DirectoryEntryType::Directory;
        return ResultSuccess;
    }

    IVirtualFilePtr file(dir->GetFile(filename.data()));
    if (file)
    {
        *out_entry_type = FileSys::DirectoryEntryType::File;
        return ResultSuccess;
    }

    IVirtualDirectoryPtr subdir(dir->GetSubdirectory(filename.data()));
    if (subdir) {
        *out_entry_type = FileSys::DirectoryEntryType::Directory;
        return ResultSuccess;
    }
    return FileSys::ResultPathNotFound;
}

void LoopProcess(Core::System & system)
{
    auto server_manager = std::make_unique<ServerManager>(system);

    const auto FileSystemProxyFactory = [&] { return std::make_shared<FSP_SRV>(system); };

    server_manager->RegisterNamedService("fsp-ldr", std::make_shared<FSP_LDR>(system));
    server_manager->RegisterNamedService("fsp:pr", std::make_shared<FSP_PR>(system));
    server_manager->RegisterNamedService("fsp-srv", std::move(FileSystemProxyFactory));
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service::FileSystem
