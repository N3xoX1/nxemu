#include "game_library.h"
#include "rom_properties.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu/settings/ui_identifiers.h>
#include <nxemu/settings/ui_settings.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/fs/path_util.h>

GameLibrary::GameLibrary() :
    m_directories(uiSettings.gameDirectories),
    m_recursive(uiSettings.gameDirectoriesRecursive)
{
}

GameLibrary::~GameLibrary()
{
    StopScan();
    StopDetails();
}

bool GameLibrary::AddDirectory(const std::string & directory)
{
    if (directory.empty() || std::find(m_directories.begin(), m_directories.end(), directory) != m_directories.end())
    {
        return false;
    }
    m_directories.push_back(directory);
    SetUISettingStringList(NXUISetting::GameDirectories, m_directories);
    SaveUISetting();
    return true;
}

bool GameLibrary::RemoveDirectory(const std::string & directory)
{
    const auto found = std::find(m_directories.begin(), m_directories.end(), directory);
    if (found == m_directories.end())
    {
        return false;
    }
    m_directories.erase(found);
    SetUISettingStringList(NXUISetting::GameDirectories, m_directories);
    m_recursive.erase(std::remove(m_recursive.begin(), m_recursive.end(), directory), m_recursive.end());
    SetUISettingStringList(NXUISetting::GameDirectoriesRecursive, m_recursive);
    SaveUISetting();
    return true;
}

bool GameLibrary::ScanSubfolders(const std::string & directory) const
{
    return std::find(m_recursive.begin(), m_recursive.end(), directory) != m_recursive.end();
}

void GameLibrary::SetScanSubfolders(const std::string & directory, bool scanSubfolders)
{
    if (ScanSubfolders(directory) == scanSubfolders)
    {
        return;
    }
    if (scanSubfolders)
    {
        m_recursive.push_back(directory);
    }
    else
    {
        m_recursive.erase(std::remove(m_recursive.begin(), m_recursive.end(), directory), m_recursive.end());
    }
    SetUISettingStringList(NXUISetting::GameDirectoriesRecursive, m_recursive);
    SaveUISetting();
}

const std::vector<std::string> & GameLibrary::RecentFiles() const
{
    return uiSettings.recentFiles;
}

void GameLibrary::AddRecentFile(const std::string & path)
{
    constexpr size_t kMaxRecentFiles = 10;
    const auto samePath = [&path](const std::string & file) {
#ifdef _WIN32
        return _stricmp(file.c_str(), path.c_str()) == 0;
#else
        return file == path;
#endif
    };
    Stringlist recentFiles = uiSettings.recentFiles;
    recentFiles.erase(std::remove_if(recentFiles.begin(), recentFiles.end(), samePath), recentFiles.end());
    recentFiles.insert(recentFiles.begin(), path);
    if (recentFiles.size() > kMaxRecentFiles)
    {
        recentFiles.resize(kMaxRecentFiles);
    }
    SetUISettingStringList(NXUISetting::RecentFiles, recentFiles);
    SaveUISetting();
}

void GameLibrary::StartScan(ISystemModules & modules)
{
    StopScan();
    {
        std::lock_guard<std::mutex> lock(m_foundMutex);
        m_found.clear();
    }
    m_stop = false;
    m_scanCompleted = false;
    m_filesStarted = 0;
    m_filesTotal = 0;
    m_scanning = true;
    m_thread = std::thread([this, &modules, directories = m_directories, recursive = m_recursive] { Scan(modules, directories, recursive); });
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

void GameLibrary::Scan(ISystemModules & modules, std::vector<std::string> directories, std::vector<std::string> recursive)
{
    std::vector<const char *> paths;
    std::unique_ptr<bool[]> scanSubfolders(new bool[directories.size() + 1]);
    for (const std::string & directory : directories)
    {
        scanSubfolders[paths.size()] = std::find(recursive.begin(), recursive.end(), directory) != recursive.end();
        paths.push_back(directory.c_str());
    }
    {
        std::lock_guard<std::mutex> lock(m_loaderMutex);
        m_scanCompleted = modules.Systemloader().ScanGameList(paths.data(), scanSubfolders.get(), static_cast<uint32_t>(paths.size()), *this) && !m_stop;
    }
    m_filesTotal = 0;
    m_scanning = false;
}

void GameLibrary::RequestDetails(ISystemModules & modules, const std::string & path)
{
    QueueDetails(modules, {path, false, 0, {}});
}

void GameLibrary::RequestDetails(ISystemModules & modules, const std::string & path, uint64_t programId, std::vector<std::string> disabledAddOns)
{
    QueueDetails(modules, {path, true, programId, std::move(disabledAddOns)});
}

bool GameLibrary::TakeDetails(GameDetails & details)
{
    std::lock_guard<std::mutex> lock(m_detailsMutex);
    if (!m_detailsReady)
    {
        return false;
    }
    details = std::move(m_details);
    m_detailsReady = false;
    return true;
}

void GameLibrary::StopDetails()
{
    {
        std::lock_guard<std::mutex> lock(m_detailsMutex);
        m_detailsStopping = true;
    }
    if (m_detailsThread.joinable())
    {
        m_detailsThread.join();
    }
    std::lock_guard<std::mutex> lock(m_detailsMutex);
    m_detailsRequests.clear();
    m_detailsStopping = false;
    m_detailsRunning = false;
    m_detailsReady = false;
}

void GameLibrary::QueueDetails(ISystemModules & modules, DetailsRequest request)
{
    std::lock_guard<std::mutex> lock(m_detailsMutex);
    m_detailsRequests.push_back(std::move(request));
    if (m_detailsRunning)
    {
        return;
    }
    if (m_detailsThread.joinable())
    {
        m_detailsThread.join();
    }
    m_detailsRunning = true;
    m_detailsThread = std::thread([this, &modules] { ReadDetails(modules); });
}

void GameLibrary::ReadDetails(ISystemModules & modules)
{
    for (;;)
    {
        DetailsRequest request;
        bool latest = false;
        {
            std::lock_guard<std::mutex> lock(m_detailsMutex);
            if (m_detailsRequests.empty())
            {
                m_detailsRunning = false;
                return;
            }
            request = std::move(m_detailsRequests.front());
            m_detailsRequests.erase(m_detailsRequests.begin());
            latest = m_detailsRequests.empty() && !m_detailsStopping;
        }
        std::lock_guard<std::mutex> loaderLock(m_loaderMutex);
        ISystemloader & loader = modules.Systemloader();
        if (request.setDisabledAddOns)
        {
            std::vector<const char *> names;
            for (const std::string & name : request.disabledAddOns)
            {
                names.push_back(name.c_str());
            }
            loader.SetDisabledAddons(request.programId, names.data(), static_cast<uint32_t>(names.size()));
        }
        if (!latest)
        {
            continue;
        }
        GameDetails details = GameDetailsReader::Read(loader, request.path);
        std::lock_guard<std::mutex> lock(m_detailsMutex);
        if (m_detailsRequests.empty())
        {
            m_details = std::move(details);
            m_detailsReady = true;
        }
    }
}

bool GameLibrary::ScanStopRequested()
{
    return m_stop;
}

void GameLibrary::ScanProgress(uint32_t filesStarted, uint32_t filesTotal)
{
    m_filesStarted = filesStarted;
    m_filesTotal = filesTotal;
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
    if (IFileSysNACPPtr control(info->GetControlMetadata()); control && control->GetDeveloperName() != nullptr)
    {
        game.publisher = control->GetDeveloperName();
    }
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(std::filesystem::path{Common::FS::ToU8String(game.path)}, error);
    game.size = error ? 0 : static_cast<uint64_t>(size);
    found.icon = RomProperties::Icon(*info);

    std::lock_guard<std::mutex> lock(m_foundMutex);
    m_found.push_back(std::move(found));
}
