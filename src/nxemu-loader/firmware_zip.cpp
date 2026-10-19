#include "firmware_zip.h"

#include <fstream>
#include <vector>

#include "ioapi.h"
#include "unzip.h"

#ifdef _WIN32
#include "iowin32.h"
#endif

namespace
{

constexpr size_t kCopyBufferSize = 1024 * 1024;
constexpr size_t kMaxEntryNameLength = 4096;

unzFile OpenZipArchive(const std::filesystem::path & zip_path)
{
#ifdef _WIN32
    // The wide functions open paths the system's code page cannot spell.
    zlib_filefunc64_def file_functions{};
    fill_win32_filefunc64W(&file_functions);
    return unzOpen2_64(zip_path.c_str(), &file_functions);
#else
    return unzOpen64(zip_path.c_str());
#endif
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

// The name the current entry is extracted under, empty to leave it out, and
// its size once extracted.
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

FirmwareZipResult ExtractFirmwareZip(const std::filesystem::path & zip_path, const std::filesystem::path & destination_directory,
                                     const std::function<std::string(std::string_view)> & nameOf,
                                     const std::function<bool(uint64_t, uint64_t)> & progress)
{
    ZipArchive zip{OpenZipArchive(zip_path)};
    if (zip.handle == nullptr)
    {
        return FirmwareZipResult::NotAnArchive;
    }

    // What there is to write, so that the progress has an end.
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
        // Closing is where the archive's checksum of the entry is verified.
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
