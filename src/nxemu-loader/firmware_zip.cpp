#include "firmware_zip.h"

#include <fstream>
#include <vector>

#include "core/file_sys/vfs/vfs.h"
#include "ioapi.h"
#include "unzip.h"

namespace
{

constexpr size_t kCopyBufferSize = 1024 * 1024;
constexpr size_t kMaxEntryNameLength = 4096;

struct ZipStream
{
    FileSys::VirtualFile file;
    uint64_t offset = 0;
};

voidpf ZCALLBACK ZipOpen(voidpf opaque, const void *, int mode)
{
    if ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER) != ZLIB_FILEFUNC_MODE_READ)
    {
        return nullptr;
    }
    return new ZipStream{*static_cast<const FileSys::VirtualFile *>(opaque)};
}

uLong ZCALLBACK ZipRead(voidpf, voidpf stream, void * buf, uLong size)
{
    ZipStream & zip = *static_cast<ZipStream *>(stream);
    const size_t read = zip.file->Read(static_cast<uint8_t *>(buf), size, zip.offset);
    zip.offset += read;
    return static_cast<uLong>(read);
}

uLong ZCALLBACK ZipWrite(voidpf, voidpf, const void *, uLong)
{
    return 0;
}

ZPOS64_T ZCALLBACK ZipTell(voidpf, voidpf stream)
{
    return static_cast<ZipStream *>(stream)->offset;
}

long ZCALLBACK ZipSeek(voidpf, voidpf stream, ZPOS64_T offset, int origin)
{
    ZipStream & zip = *static_cast<ZipStream *>(stream);
    switch (origin)
    {
    case ZLIB_FILEFUNC_SEEK_SET: zip.offset = offset; break;
    case ZLIB_FILEFUNC_SEEK_CUR: zip.offset += offset; break;
    case ZLIB_FILEFUNC_SEEK_END: zip.offset = zip.file->GetSize() + offset; break;
    default: return -1;
    }
    return 0;
}

int ZCALLBACK ZipClose(voidpf, voidpf stream)
{
    delete static_cast<ZipStream *>(stream);
    return 0;
}

int ZCALLBACK ZipError(voidpf, voidpf)
{
    return 0;
}

unzFile OpenZipArchive(const FileSys::VirtualFile & zip_file)
{
    zlib_filefunc64_def file_functions{ZipOpen, ZipRead, ZipWrite, ZipTell, ZipSeek, ZipClose, ZipError, const_cast<FileSys::VirtualFile *>(&zip_file)};
    return unzOpen2_64(zip_file->GetName().c_str(), &file_functions);
}

struct ZipArchive
{
    unzFile handle = nullptr;
    ~ZipArchive()
    {
        if (handle != nullptr)
        {
            unzClose(handle);
        }
    }
};

bool CurrentEntry(unzFile zip, const std::function<std::string(std::string_view)> & nameOf, std::string & name, uint64_t & size)
{
    unz_file_info64 info{};
    if (unzGetCurrentFileInfo64(zip, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK || info.size_filename > kMaxEntryNameLength)
    {
        return false;
    }
    std::string entry_name(info.size_filename, '\0');
    if (unzGetCurrentFileInfo64(zip, &info, entry_name.data(), static_cast<uLong>(entry_name.size()), nullptr, 0, nullptr, 0) != UNZ_OK)
    {
        return false;
    }
    const bool directory = !entry_name.empty() && (entry_name.back() == '/' || entry_name.back() == '\\');
    name = directory ? std::string() : nameOf(entry_name);
    size = info.uncompressed_size;
    return true;
}

}

FirmwareZipResult ExtractFirmwareZip(const FileSys::VirtualFile & zip_file, const std::filesystem::path & destination_directory,
                                     const std::function<std::string(std::string_view)> & nameOf,
                                     const std::function<bool(uint64_t, uint64_t)> & progress)
{
    ZipArchive zip{OpenZipArchive(zip_file)};
    if (zip.handle == nullptr)
    {
        return FirmwareZipResult::NotAnArchive;
    }

    uint64_t total = 0;
    for (int entry = unzGoToFirstFile(zip.handle); entry != UNZ_END_OF_LIST_OF_FILE; entry = unzGoToNextFile(zip.handle))
    {
        std::string name;
        uint64_t size = 0;
        if (entry != UNZ_OK || !CurrentEntry(zip.handle, nameOf, name, size))
        {
            return FirmwareZipResult::Failed;
        }
        if (!name.empty())
        {
            total += size;
        }
    }

    std::vector<char> buffer(kCopyBufferSize);
    uint64_t done = 0;
    for (int entry = unzGoToFirstFile(zip.handle); entry != UNZ_END_OF_LIST_OF_FILE; entry = unzGoToNextFile(zip.handle))
    {
        std::string name;
        uint64_t size = 0;
        if (entry != UNZ_OK || !CurrentEntry(zip.handle, nameOf, name, size))
        {
            return FirmwareZipResult::Failed;
        }
        if (name.empty())
        {
            continue;
        }
        if (unzOpenCurrentFile(zip.handle) != UNZ_OK)
        {
            return FirmwareZipResult::Failed;
        }
        std::ofstream output(destination_directory / std::filesystem::path(std::u8string(name.begin(), name.end())), std::ios::binary | std::ios::trunc);
        FirmwareZipResult result = output ? FirmwareZipResult::Extracted : FirmwareZipResult::Failed;
        while (result == FirmwareZipResult::Extracted)
        {
            const int read = unzReadCurrentFile(zip.handle, buffer.data(), static_cast<unsigned>(buffer.size()));
            if (read == 0)
            {
                break;
            }
            if (read < 0 || !output.write(buffer.data(), read))
            {
                result = FirmwareZipResult::Failed;
            }
            else if (!progress(done += static_cast<uint64_t>(read), total))
            {
                result = FirmwareZipResult::Cancelled;
            }
        }
        output.close();
        if (unzCloseCurrentFile(zip.handle) != UNZ_OK && result == FirmwareZipResult::Extracted)
        {
            result = FirmwareZipResult::Failed;
        }
        if (result == FirmwareZipResult::Extracted && !output)
        {
            result = FirmwareZipResult::Failed;
        }
        if (result != FirmwareZipResult::Extracted)
        {
            return result;
        }
    }
    return FirmwareZipResult::Extracted;
}
