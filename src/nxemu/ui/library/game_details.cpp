#include "game_details.h"
#include <common/std_string.h>
#include <filesystem>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/fs/path_util.h>

namespace
{
std::string ExistingDirectory(const std::filesystem::path & directory)
{
    std::error_code error;
    return std::filesystem::is_directory(directory, error) ? Common::FS::PathToUTF8String(directory) : std::string();
}
} // namespace

std::string GameDetailsReader::SaveDirectory(uint64_t programId)
{
    if (programId == 0)
    {
        return {};
    }
    const std::string title = stdstr_f("%016llX", static_cast<unsigned long long>(programId));
    const std::filesystem::path users = Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir) / "user/save/0000000000000000";
    std::error_code error;
    for (std::filesystem::directory_iterator user(users, error), end; !error && user != end; user.increment(error))
    {
        const std::string directory = ExistingDirectory(user->path() / title);
        if (!directory.empty())
        {
            return directory;
        }
    }
    return {};
}
