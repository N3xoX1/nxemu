#include "library_files.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <common/sha256.h>
#include <system_error>
#include <yuzu_common/fs/fs_util.h>

namespace
{
std::string LowerExtension(const std::filesystem::path & path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return extension;
}

std::string Signature(const std::filesystem::directory_entry & entry, std::error_code & error)
{
    const auto size = entry.file_size(error);
    if (error)
    {
        return {};
    }
    const auto modified = entry.last_write_time(error);
    if (error)
    {
        return {};
    }
    return std::to_string(size) + ":" + std::to_string(static_cast<long long>(modified.time_since_epoch().count()));
}

template <typename Iterator, typename Accept>
LibraryListing List(const std::filesystem::path & directory, Accept && accept)
{
    LibraryListing listing;
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::status(directory, error);
    if (status.type() == std::filesystem::file_type::not_found)
    {
        listing.complete = true;
        listing.missing = true;
        return listing;
    }
    if (error || !std::filesystem::is_directory(status))
    {
        return listing;
    }

    // A file that cannot be looked at, or a directory that stops reading,
    // leaves the listing incomplete: the files it did give are still listed.
    bool complete = true;
    Iterator itr(directory, error);
    for (; !error && itr != Iterator(); itr.increment(error))
    {
        std::error_code fileError;
        if (!itr->is_regular_file(fileError) || !accept(itr->path()))
        {
            complete = complete && !fileError;
            continue;
        }
        std::string signature = Signature(*itr, fileError);
        if (fileError)
        {
            complete = false;
            continue;
        }
        listing.files.push_back({itr->path(), std::move(signature)});
    }
    std::sort(listing.files.begin(), listing.files.end(),
              [](const LibraryFile & left, const LibraryFile & right) { return left.path.native() < right.path.native(); });
    listing.complete = complete && !error;
    return listing;
}
} // namespace

bool LibraryFiles::IsGame(const std::filesystem::path & path)
{
    const std::string extension = LowerExtension(path);
    return IsContainer(path) || extension == ".nso" || extension == ".nro" || extension == ".kip";
}

bool LibraryFiles::IsContainer(const std::filesystem::path & path)
{
    const std::string extension = LowerExtension(path);
    return extension == ".dnsp" || extension == ".dxci" || extension == ".nca";
}

LibraryListing LibraryFiles::ListGames(const std::filesystem::path & directory)
{
    return List<std::filesystem::directory_iterator>(directory, IsGame);
}

LibraryListing LibraryFiles::ListContainers(const std::filesystem::path & directory)
{
    return List<std::filesystem::recursive_directory_iterator>(directory, IsContainer);
}

LibraryListing LibraryFiles::ListAll(const std::filesystem::path & directory)
{
    return List<std::filesystem::recursive_directory_iterator>(directory, [](const std::filesystem::path &) { return true; });
}

std::string LibraryFiles::Fingerprint(const LibraryListing & listing)
{
    std::string contents;
    for (const LibraryFile & file : listing.files)
    {
        contents += Common::FS::PathToUTF8String(file.path) + '\n' + file.signature + '\n';
    }
    return sha256(std::move(contents));
}

uint64_t LibraryFiles::SizeOf(const std::string & signature)
{
    // A signature starts with the size: see Signature().
    return std::strtoull(signature.c_str(), nullptr, 10);
}
