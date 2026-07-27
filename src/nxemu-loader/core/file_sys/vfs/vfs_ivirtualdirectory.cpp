#include "vfs_ivirtualdirectory.h"
#include "yuzu_common/yuzu_assert.h"
#include <memory>
#include <string>

VfsVirtualDirectory::VfsVirtualDirectory(IVirtualDirectory * directory) :
    m_directory(directory)
{
}

VfsVirtualDirectory::~VfsVirtualDirectory()
{
    if (m_directory != nullptr)
    {
        m_directory->Release();
    }
}

std::vector<FileSys::VirtualFile> VfsVirtualDirectory::GetFiles() const
{
    UNIMPLEMENTED();
    return {};
}

std::vector<FileSys::VirtualDir> VfsVirtualDirectory::GetSubdirectories() const
{
    UNIMPLEMENTED();
    return {};
}

bool VfsVirtualDirectory::IsWritable() const
{
    UNIMPLEMENTED();
    return false;
}

bool VfsVirtualDirectory::IsReadable() const
{
    UNIMPLEMENTED();
    return true;
}

std::string VfsVirtualDirectory::GetName() const
{
    UNIMPLEMENTED();
    return "";
}

FileSys::VirtualDir VfsVirtualDirectory::GetParentDirectory() const
{
    UNIMPLEMENTED();
    return nullptr;
}

FileSys::VirtualDir VfsVirtualDirectory::CreateSubdirectory(std::string_view name)
{
    UNIMPLEMENTED();
    return nullptr;
}

FileSys::VirtualFile VfsVirtualDirectory::CreateFile(std::string_view name)
{
    UNIMPLEMENTED();
    return nullptr;
}

bool VfsVirtualDirectory::DeleteSubdirectory(std::string_view)
{
    UNIMPLEMENTED();
    return false;
}

bool VfsVirtualDirectory::DeleteFile(std::string_view name)
{
    if (m_directory == nullptr)
    {
        return false;
    }
    return m_directory->DeleteFile(std::string(name).c_str());
}

bool VfsVirtualDirectory::Rename(std::string_view)
{
    UNIMPLEMENTED();
    return false;
}

FileSys::VirtualFile VfsVirtualDirectory::GetFile(std::string_view name) const
{
    UNIMPLEMENTED();
    return nullptr;
}

FileSys::VirtualDir VfsVirtualDirectory::GetSubdirectory(std::string_view name) const
{
    UNIMPLEMENTED();
    return nullptr;
}

FileSys::VirtualFile VfsVirtualDirectory::GetFileRelative(std::string_view path) const
{
    UNIMPLEMENTED();
    return nullptr;
}

FileSys::VirtualDir VfsVirtualDirectory::GetDirectoryRelative(std::string_view path) const
{
    UNIMPLEMENTED();
    return nullptr;
}
