#include "game_cache.h"
#include <common/std_string.h>

std::filesystem::path GameCache::Directory(const std::filesystem::path & root, uint64_t programId)
{
    if (programId == 0 || root.empty())
    {
        return {};
    }
    return root / std::string(stdstr_f("%016llx", static_cast<unsigned long long>(programId)));
}

bool GameCache::Clear(const std::filesystem::path & root, uint64_t programId, std::string & error)
{
    error.clear();
    const std::filesystem::path directory = Directory(root, programId);
    if (directory.empty())
    {
        error = "Invalid game cache directory";
        return false;
    }
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::symlink_status(directory, ec);
    if (ec == std::errc::no_such_file_or_directory)
    {
        return true;
    }
    if (ec)
    {
        error = ec.message();
        return false;
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status))
    {
        error = "Invalid game cache directory";
        return false;
    }
    std::filesystem::remove_all(directory, ec);
    if (ec)
    {
        error = ec.message();
    }
    return !ec;
}
