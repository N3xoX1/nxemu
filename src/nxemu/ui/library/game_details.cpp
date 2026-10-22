#include "game_details.h"
#include "rom_properties.h"
#include <cinttypes>
#include <common/json.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/system_loader.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/fs/path_util.h>

namespace
{
std::string TitleDirectoryName(uint64_t programId, bool upperCase)
{
    char name[17];
    std::snprintf(name, sizeof(name), upperCase ? "%016llX" : "%016llx", static_cast<unsigned long long>(programId));
    return name;
}

std::string ExistingDirectory(const std::filesystem::path & directory)
{
    std::error_code error;
    return std::filesystem::is_directory(directory, error) ? Common::FS::PathToUTF8String(directory) : std::string();
}

// A game's saves are kept per user of the emulated console. The first user
// that has any is the one shown: there is seldom more than one.
std::string FindSaveDirectory(uint64_t programId)
{
    const std::filesystem::path users = Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir) / "user/save/0000000000000000";
    std::error_code error;
    for (std::filesystem::directory_iterator user(users, error), end; !error && user != end; user.increment(error))
    {
        const std::string directory = ExistingDirectory(user->path() / TitleDirectoryName(programId, true));
        if (!directory.empty())
        {
            return directory;
        }
    }
    return {};
}

GameAddOn::Kind KindOf(GamePatchType type)
{
    switch (type)
    {
    case GamePatchType::Update: return GameAddOn::Kind::Update;
    case GamePatchType::AddOnContent: return GameAddOn::Kind::AddOnContent;
    default: return GameAddOn::Kind::Mod;
    }
}
} // namespace

std::string GameDetailsReader::SaveDirectory(uint64_t programId)
{
    return programId == 0 ? std::string() : FindSaveDirectory(programId);
}

GameDetails GameDetailsReader::Read(ISystemloader & loader, const std::string & path)
{
    GameDetails details;
    details.path = path;
    IRomInfoPtr info(loader.RomInfo(path.c_str(), 0, 0));
    uint64_t programId = 0;
    if (!info || info->GetFileType() == LoaderFileType::Unknown || info->GetFileType() == LoaderFileType::Error ||
        info->ReadProgramId(programId) != LoaderResultStatus::Success || programId == 0)
    {
        return details;
    }
    details.programId = programId;
    std::string publisher, baseVersion;
    RomProperties::Release(*info, details.version, publisher, baseVersion);

    std::vector<GamePatchInfo> patches(info->GetGamePatches(nullptr, 0));
    patches.resize(info->GetGamePatches(patches.data(), static_cast<uint32_t>(patches.size())));
    for (const GamePatchInfo & patch : patches)
    {
        details.addOns.push_back({KindOf(patch.type), patch.key, patch.name, patch.version, patch.enabled, {}});
        if (patch.type == GamePatchType::AddOnContent)
        {
            std::vector<GameAddOnContentInfo> contents(info->GetAddOnContents(nullptr, 0));
            contents.resize(info->GetAddOnContents(contents.data(), static_cast<uint32_t>(contents.size())));
            for (const GameAddOnContentInfo & content : contents)
            {
                details.addOns.back().items.push_back({content.titleId, content.version, content.size});
            }
        }
    }


    details.saveDirectory = SaveDirectory(programId);
    details.cacheDirectory = ExistingDirectory(Common::FS::GetYuzuPath(Common::FS::YuzuPath::ShaderDir) / TitleDirectoryName(programId, false));
    details.modDirectory = Common::FS::PathToUTF8String(Common::FS::GetYuzuPath(Common::FS::YuzuPath::LoadDir) / TitleDirectoryName(programId, true));
    return details;
}

std::vector<std::string> GameDetailsReader::DisabledAfterToggle(const GameDetails & details, size_t index)
{
    std::vector<std::string> disabled;
    if (index >= details.addOns.size())
    {
        return disabled;
    }
    const GameAddOn & toggled = details.addOns[index];
    const bool enabling = !toggled.enabled;
    for (size_t i = 0; i < details.addOns.size(); i++)
    {
        const GameAddOn & addOn = details.addOns[i];
        bool enabled = i == index ? enabling : addOn.enabled;
        if (i != index && enabling && toggled.kind == GameAddOn::Kind::Update && addOn.kind == GameAddOn::Kind::Update)
        {
            enabled = false;
        }
        if (!enabled)
        {
            disabled.push_back(addOn.key);
        }
    }
    return disabled;
}
