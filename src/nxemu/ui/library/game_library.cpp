#include "game_library.h"
#include "rom_properties.h"
#include <filesystem>
#include <nxemu-core/modules/system_modules.h>
#include <settings/ui_settings.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/fs/path_util.h>

GameLibrary::~GameLibrary()
{
    StopScan();
}

const std::vector<std::string> & GameLibrary::Directories() const
{
    return uiSettings.gameDirectories;
}

void GameLibrary::StartScan(ISystemModules & modules)
{
    StopScan();
    {
        std::lock_guard<std::mutex> lock(m_foundMutex);
        m_found.clear();
    }
    m_stop = false;
    m_scanning = true;
    m_thread = std::thread([this, &modules, directories = Directories()] { Scan(modules, directories); });
}

void GameLibrary::StopScan()
{
    m_stop = true;
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    m_scanning = false;
}

bool GameLibrary::TakeFound(std::vector<FoundGame> & games)
{
    std::lock_guard<std::mutex> lock(m_foundMutex);
    if (m_found.empty())
    {
        return false;
    }
    games = std::move(m_found);
    m_found.clear();
    return true;
}

void GameLibrary::Scan(ISystemModules & modules, std::vector<std::string> directories)
{
    std::vector<const char *> paths;
    for (const std::string & directory : directories)
    {
        paths.push_back(directory.c_str());
    }
    modules.Systemloader().ScanGameList(paths.data(), static_cast<uint32_t>(paths.size()), *this);
    m_scanning = false;
}

bool GameLibrary::ScanStopRequested()
{
    return m_stop;
}

void GameLibrary::GameFound(const char * path, IRomInfo * romInfo, bool /*changed*/)
{
    IRomInfoPtr info(romInfo);
    if (!info)
    {
        return;
    }
    FoundGame found;
    GameEntry & game = found.game;
    game.path = path;
    if (info->ReadProgramId(game.programId) != LoaderResultStatus::Success)
    {
        game.programId = 0;
    }
    game.title = RomProperties::Title(*info);
    if (game.title.empty())
    {
        game.title = Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(game.path)}.filename());
    }
    game.version = RomProperties::Version(*info);
    found.icon = RomProperties::Icon(*info);

    std::lock_guard<std::mutex> lock(m_foundMutex);
    m_found.push_back(std::move(found));
}
