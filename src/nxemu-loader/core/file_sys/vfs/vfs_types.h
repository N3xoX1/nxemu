// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include "yuzu_common/common_types.h"
#include <nxemu-module-spec/system_loader.h>

namespace FileSys {

class VfsDirectory;
class VfsFile;
class VfsFilesystem;

// Declarations for Vfs* pointer types

using VirtualDir = std::shared_ptr<VfsDirectory>;
using VirtualFile = std::shared_ptr<VfsFile>;
using VirtualFilesystem = std::shared_ptr<VfsFilesystem>;

struct FileTimeStampRaw {
    uint64_t created{};
    uint64_t accessed{};
    uint64_t modified{};
    uint64_t padding{};
};

} // namespace FileSys

class VirtualDirectoryListImpl :
    public IVirtualDirectoryList
{
public:
    VirtualDirectoryListImpl();
    VirtualDirectoryListImpl(std::vector<FileSys::VirtualDir> && directories);
    VirtualDirectoryListImpl(VirtualDirectoryListImpl && other) noexcept;
    ~VirtualDirectoryListImpl();

    // IVirtualDirectoryList
    uint32_t GetSize() const override;
    IVirtualDirectory * GetItem(uint32_t index) const override;
    void Release() override;

private:
    VirtualDirectoryListImpl(const VirtualDirectoryListImpl &) = delete;
    VirtualDirectoryListImpl & operator=(const VirtualDirectoryListImpl &) = delete;

    std::vector<FileSys::VirtualDir> m_directories;
};

class VirtualDirectoryImpl :
    public IVirtualDirectory
{
public:
    VirtualDirectoryImpl();
    VirtualDirectoryImpl(FileSys::VirtualDir & directory);
    VirtualDirectoryImpl(VirtualDirectoryImpl && other) noexcept;
    ~VirtualDirectoryImpl();

    VirtualDirectoryImpl & operator=(VirtualDirectoryImpl && other) noexcept;

    FileSys::VfsDirectory * operator->() const;
    FileSys::VfsDirectory & operator*() const;
    operator bool() const;

    // IVirtualDirectory
    IVirtualDirectoryList * GetSubdirectories() const override;
    IVirtualDirectory * CreateSubdirectory(const char * path) const override;
    IVirtualDirectory * GetDirectoryRelative(const char * path) const override;
    IVirtualDirectory * GetSubdirectory(const char * name) const override;
    IVirtualDirectory * Duplicate() override;
    IVirtualFileList * GetFiles() const override;
    IVirtualFile * CreateFile(const char * name) const override;
    IVirtualFile * GetFile(const char * name) const override;
    IVirtualFile * GetFileRelative(const char * relative_path) const override;
    IVirtualFile * OpenFile(const char * path, VirtualFileOpenMode perms) override;
    bool DeleteFile(const char * name) const override;
    const char * GetName() const override;
    void Release() override;

private:
    VirtualDirectoryImpl(const VirtualDirectoryImpl &) = delete;
    VirtualDirectoryImpl & operator=(const VirtualDirectoryImpl &) = delete;

    FileSys::VirtualDir m_directory;
    mutable std::string m_cachedName;
};

class VirtualFileListImpl :
    public IVirtualFileList
{
public:
    VirtualFileListImpl();
    VirtualFileListImpl(std::vector<FileSys::VirtualFile> && files);
    VirtualFileListImpl(VirtualFileListImpl && other) noexcept;
    ~VirtualFileListImpl();

    // IVirtualFileList
    uint32_t GetSize() const override;
    IVirtualFile * GetItem(uint32_t index) const override;
    void Release() override;

private:
    VirtualFileListImpl(const VirtualFileListImpl &) = delete;
    VirtualFileListImpl & operator=(const VirtualFileListImpl &) = delete;

    std::vector<FileSys::VirtualFile> m_files;
};

class VirtualFileImpl :
    public IVirtualFile
{
public:
    VirtualFileImpl();
    VirtualFileImpl(FileSys::VirtualFile & file);
    VirtualFileImpl(VirtualFileImpl && other) noexcept;
    ~VirtualFileImpl();

    VirtualFileImpl & operator=(VirtualFileImpl && other) noexcept;

    FileSys::VfsFile* operator->() const;
    FileSys::VfsFile& operator*() const;
    operator bool() const;

    // IVirtualFile
    uint64_t GetSize() const override;
    const char * GetName() const override;
    const char * GetFullPath() const override;
    bool Resize(uint64_t size) override;
    uint64_t ReadBytes(uint8_t * data, uint64_t datalen, uint64_t offset) override;
    uint64_t WriteBytes(const uint8_t * data, uint64_t datalen, uint64_t offset) override;
    IVirtualDirectory * ExtractRomFS() override;
    IVirtualDirectory * GetContainingDirectory() const override;
    IVirtualFile * Duplicate() override;
    bool Rename(const char * name) override;
    void Release() override;

private:
    VirtualFileImpl(const VirtualFileImpl &) = delete;
    VirtualFileImpl & operator=(const VirtualFileImpl &) = delete;

    FileSys::VirtualFile m_file;
    mutable std::string m_cachedName;
    mutable std::string m_cachedFullPath;
};
