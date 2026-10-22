#pragma once

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

// Extracts files of a ZIP archive into a directory, without the directories
// the archive keeps them in. nameOf gives the name to extract an entry under,
// or nothing to leave it out; a name is used as it is, so it must not hold a
// path. progress is told how many of the bytes to write are written, and
// stops the extraction by returning false.
FirmwareZipResult ExtractFirmwareZip(const std::filesystem::path & zip_path, const std::filesystem::path & destination_directory,
                                     const std::function<std::string(std::string_view)> & nameOf,
                                     const std::function<bool(uint64_t, uint64_t)> & progress);
