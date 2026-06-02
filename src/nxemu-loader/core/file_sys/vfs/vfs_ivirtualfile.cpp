#include "vfs_ivirtualfile.h"
#include "vfs_ivirtualdirectory.h"
#include "yuzu_common/yuzu_assert.h"
#include <memory>

VfsVirtualFile::VfsVirtualFile(IVirtualFile * file) :
    m_file(file)
{
}

VfsVirtualFile::~VfsVirtualFile()
{
    if (m_file != nullptr)
    {
        m_file->Release();
    }
}

std::string VfsVirtualFile::GetName() const
{
    if (m_file == nullptr)
    {
        return {};
    }

    const char * const name = m_file->GetName();
    return name != nullptr ? name : std::string{};
}

std::string VfsVirtualFile::GetExtension() const
{
    return VfsFile::GetExtension();
}

std::size_t VfsVirtualFile::GetSize() const
{
    return m_file->GetSize();
}

bool VfsVirtualFile::Resize(std::size_t new_size)
{
    UNIMPLEMENTED();
    return false;
}

FileSys::VirtualDir VfsVirtualFile::GetContainingDirectory() const
{
    IVirtualDirectory * const dir = m_file->GetContainingDirectory();
    if (dir == nullptr)
    {
        return {};
    }
    return std::make_shared<VfsVirtualDirectory>(dir);
}

bool VfsVirtualFile::IsWritable() const
{
    UNIMPLEMENTED();
    return false;
}

bool VfsVirtualFile::IsReadable() const
{
    return m_file != nullptr;
}

std::size_t VfsVirtualFile::Read(u8 * data, std::size_t length, std::size_t offset) const
{
    return m_file->ReadBytes(data, length, offset);
}

std::size_t VfsVirtualFile::Write(const u8 * data, std::size_t length, std::size_t offset)
{
    UNIMPLEMENTED();
    return 0;
}

bool VfsVirtualFile::Rename(std::string_view name)
{
    UNIMPLEMENTED();
    return false;
}
