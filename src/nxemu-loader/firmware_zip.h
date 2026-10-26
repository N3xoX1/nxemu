#pragma once

#include "core/file_sys/vfs/vfs_types.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

enum class FirmwareZipResult
{
    Extracted,
    NotAnArchive,
    Cancelled,
    Failed,
};

FirmwareZipResult ExtractFirmwareZip(const FileSys::VirtualFile & zip_file, const std::filesystem::path & destination_directory,
                                     const std::function<std::string(std::string_view)> & nameOf,
                                     const std::function<bool(uint64_t, uint64_t)> & progress);
