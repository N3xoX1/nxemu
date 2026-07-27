#include "vfs_types.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/savedata_factory.h"
#include <yuzu_common/yuzu_assert.h>

VirtualDirectoryListImpl::VirtualDirectoryListImpl(std::vector<FileSys::VirtualDir> && directories) :
    m_directories(std::move(directories))
{
}

VirtualDirectoryListImpl::~VirtualDirectoryListImpl()
{
}

uint32_t VirtualDirectoryListImpl::GetSize() const
{
    return (uint32_t)m_directories.size();
}

IVirtualDirectory * VirtualDirectoryListImpl::GetItem(uint32_t index) const
{
    FileSys::VirtualDir dir = m_directories[index];
    return std::make_unique<VirtualDirectoryImpl>(dir).release();
}

void VirtualDirectoryListImpl::Release()
{
    delete this;
}

VirtualDirectoryImpl::VirtualDirectoryImpl(FileSys::VirtualDir & directory) :
    m_directory(directory)
{
}

VirtualDirectoryImpl::~VirtualDirectoryImpl()
{
}

IVirtualDirectoryList * VirtualDirectoryImpl::GetSubdirectories() const
{
    if (m_directory.get() == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<VirtualDirectoryListImpl>(m_directory->GetSubdirectories()).release();
}

IVirtualDirectory * VirtualDirectoryImpl::CreateSubdirectory(const char * path) const
{
    if (m_directory.get() == nullptr)
    {
        return nullptr;
    }
    FileSys::VirtualDir dir = m_directory->CreateSubdirectory(path);
    if (dir.get() == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<VirtualDirectoryImpl>(dir).release();
}

IVirtualDirectory * VirtualDirectoryImpl::GetDirectoryRelative(const char * path) const
{
    if (m_directory.get() == nullptr)
    {
        return nullptr;
    }
    FileSys::VirtualDir dir = m_directory->GetDirectoryRelative(path);
    if (dir.get() == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<VirtualDirectoryImpl>(dir).release();
}

IVirtualDirectory * VirtualDirectoryImpl::GetSubdirectory(const char * path) const
{
    if (m_directory.get() == nullptr)
    {
        return nullptr;
    }
    FileSys::VirtualDir dir = m_directory->GetSubdirectory(path);
    if (dir.get() == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<VirtualDirectoryImpl>(dir).release();
}

IVirtualDirectory * VirtualDirectoryImpl::Duplicate()
{
    return std::make_unique<VirtualDirectoryImpl>(m_directory).release();
}

IVirtualFileList * VirtualDirectoryImpl::GetFiles() const
{
    if (m_directory.get() == nullptr)
    {
        return nullptr;
    }
    std::vector<FileSys::VirtualFile> files = m_directory->GetFiles();
    std::erase_if(files, [](const auto & file)
    {
        return file->GetName() == FileSys::GetSaveDataSizeFileName();
    });
    return std::make_unique<VirtualFileListImpl>(m_directory->GetFiles()).release();
}

IVirtualFile * VirtualDirectoryImpl::CreateFile(const char * name) const
{
    if (m_directory.get() == nullptr)
    {
        return nullptr;
    }
    FileSys::VirtualFile file = m_directory->CreateFile(name);
    if (file.get() == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<VirtualFileImpl>(file).release();
}

IVirtualFile * VirtualDirectoryImpl::GetFile(const char * name) const
{
    if (m_directory.get() == nullptr)
    {
        return nullptr;
    }
    FileSys::VirtualFile file = m_directory->GetFile(name);
    if (file.get() == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<VirtualFileImpl>(file).release();
}

IVirtualFile * VirtualDirectoryImpl::GetFileRelative(const char * name) const
{
    if (m_directory.get() == nullptr)
    {
        return nullptr;
    }
    FileSys::VirtualFile file = m_directory->GetFileRelative(name);
    if (file.get() == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<VirtualFileImpl>(file).release();
}

IVirtualFile * VirtualDirectoryImpl::OpenFile(const char * path, VirtualFileOpenMode perms)
{
    if (m_directory.get() == nullptr)
    {
        return nullptr;
    }
    UNIMPLEMENTED();
    return nullptr;
}

bool VirtualDirectoryImpl::DeleteFile(const char * name) const
{
    if (m_directory.get() == nullptr)
    {
        return false;
    }
    return m_directory->DeleteFile(name);
}

const char * VirtualDirectoryImpl::GetName() const
{
    m_cachedName = m_directory->GetName();
    return m_cachedName.c_str();
}

void VirtualDirectoryImpl::Release()
{
    delete this;
}

VirtualFileListImpl::VirtualFileListImpl(std::vector<FileSys::VirtualFile> && files) :
    m_files(std::move(files))
{
}

VirtualFileListImpl::~VirtualFileListImpl()
{
}

uint32_t VirtualFileListImpl::GetSize() const
{
    return (uint32_t)m_files.size();
}

IVirtualFile * VirtualFileListImpl::GetItem(uint32_t index) const
{
    FileSys::VirtualFile file = m_files[index];
    return std::make_unique<VirtualFileImpl>(file).release();
}

void VirtualFileListImpl::Release()
{
    delete this;
}

VirtualFileImpl::VirtualFileImpl(FileSys::VirtualFile & file) :
    m_file(file)
{
}

VirtualFileImpl::~VirtualFileImpl()
{
}

uint64_t VirtualFileImpl::GetSize() const
{
    if (m_file)
    {
        return m_file->GetSize();
    }
    return 0;
}

const char * VirtualFileImpl::GetName() const
{
    m_cachedName = m_file->GetName();
    return m_cachedName.c_str();
}

const char * VirtualFileImpl::GetFullPath() const
{
    if (!m_file)
    {
        m_cachedFullPath.clear();
        return m_cachedFullPath.c_str();
    }
    m_cachedFullPath = m_file->GetFullPath();
    return m_cachedFullPath.c_str();
}

bool VirtualFileImpl::Resize(uint64_t size)
{
    if (m_file)
    {
        return m_file->Resize(size);
    }
    return 0;
}

uint64_t VirtualFileImpl::ReadBytes(uint8_t * data, uint64_t datalen, uint64_t offset)
{
    if (m_file)
    {
        return m_file->Read(data, datalen, offset);
    }
    return 0;
}

uint64_t VirtualFileImpl::WriteBytes(const uint8_t * data, uint64_t datalen, uint64_t offset)
{
    if (m_file)
    {
        return m_file->Write(data, datalen, offset);
    }
    return 0;
}

IVirtualDirectory * VirtualFileImpl::ExtractRomFS()
{
    FileSys::VirtualDir dir = FileSys::ExtractRomFS(m_file);
    if (dir.get() == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<VirtualDirectoryImpl>(dir).release();
}

IVirtualDirectory * VirtualFileImpl::GetContainingDirectory() const
{
    if (!m_file)
    {
        return nullptr;
    }
    FileSys::VirtualDir dir = m_file->GetContainingDirectory();
    if (dir.get() == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<VirtualDirectoryImpl>(dir).release();
}

IVirtualFile * VirtualFileImpl::Duplicate()
{
    return std::make_unique<VirtualFileImpl>(m_file).release();
}

bool VirtualFileImpl::Rename(const char * name)
{
    if (!m_file || name == nullptr)
    {
        return false;
    }
    return m_file->Rename(name);
}

void VirtualFileImpl::Release()
{
    delete this;
}