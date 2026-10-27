#include "game_library.h"
#include "rom_properties.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <exception>
#include <utility>
#include <yuzu_common/logging/log.h>
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

void GameLibrary::StartScan(ISystemloader & loader)
{
    // The caller must join this worker before releasing the borrowed loader.
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
    try
    {
        m_thread = std::thread([this, &loader, directories = m_directories, recursive = m_recursive] {
            Scan(loader, directories, recursive);
        });
    }
    catch (const std::exception & error)
    {
        LOG_ERROR(Frontend, "Could not start game library scan: {}", error.what());
        m_scanning = false;
    }
    catch (...)
    {
        LOG_ERROR(Frontend, "Could not start game library scan");
        m_scanning = false;
    }
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

void GameLibrary::Scan(ISystemloader & loader, std::vector<std::string> directories, std::vector<std::string> recursive)
{
    try
    {
        std::vector<const char *> paths;
        paths.reserve(directories.size());
        // ScanGameList consumes the flags synchronously, before these owners die.
        std::unique_ptr<bool[]> subfolders(new bool[directories.size() + 1]);
        for (const std::string & directory : directories)
        {
            subfolders[paths.size()] = std::find(recursive.begin(), recursive.end(), directory) != recursive.end();
            paths.push_back(directory.c_str());
        }
        std::lock_guard<std::mutex> lock(m_loaderMutex);
        m_scanCompleted = loader.ScanGameList(paths.data(), subfolders.get(), static_cast<uint32_t>(paths.size()), *this) && !m_stop;
    }
    catch (const std::exception & error)
    {
        LOG_ERROR(Frontend, "Game library scan failed: {}", error.what());
        m_scanCompleted = false;
    }
    catch (...)
    {
        LOG_ERROR(Frontend, "Game library scan failed with an unknown exception");
        m_scanCompleted = false;
    }
    m_filesTotal = 0;
    m_scanning = false;
}

void GameLibrary::RequestDetails(ISystemloader & loader, const std::string & path)
{
    QueueDetails(loader, {path, false, 0, {}});
}

void GameLibrary::RequestDetails(ISystemloader & loader, const std::string & path, uint64_t programId, std::vector<std::string> disabledAddOns)
{
    QueueDetails(loader, {path, true, programId, std::move(disabledAddOns)});
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
        m_detailsRequests.clear();
    }
    if (m_detailsThread.joinable())
    {
        m_detailsThread.join();
    }
    std::lock_guard<std::mutex> lock(m_detailsMutex);
    m_detailsStopping = false;
    m_detailsRunning = false;
    m_detailsReady = false;
}

void GameLibrary::QueueDetails(ISystemloader & loader, DetailsRequest request)
{
    // Called on the frontend thread; the worker never enqueues new requests.
    std::unique_lock<std::mutex> lock(m_detailsMutex);
    if (m_detailsStopping)
    {
        return;
    }
    m_detailsRequests.push_back(std::move(request));
    m_detailsReady = false;
    if (m_detailsRunning)
    {
        return;
    }
    // Never join while holding the mutex the worker needs to exit.
    lock.unlock();
    if (m_detailsThread.joinable())
    {
        m_detailsThread.join();
    }
    lock.lock();
    m_detailsRunning = true;
    try
    {
        m_detailsThread = std::thread([this, &loader] { ReadDetails(loader); });
    }
    catch (const std::exception & error)
    {
        LOG_ERROR(Frontend, "Could not start game details worker: {}", error.what());
        m_detailsRunning = false;
        m_detailsRequests.clear();
    }
    catch (...)
    {
        LOG_ERROR(Frontend, "Could not start game details worker");
        m_detailsRunning = false;
        m_detailsRequests.clear();
    }
}

void GameLibrary::ReadDetails(ISystemloader & loader)
{
    try
    {
        for (;;)
        {
            DetailsRequest request;
            bool latest = false;
            {
                std::lock_guard<std::mutex> lock(m_detailsMutex);
                if (m_detailsStopping || m_detailsRequests.empty())
                {
                    // Publish completion under the same lock QueueDetails uses:
                    // a newly queued request must never be left without a worker.
                    m_detailsRunning = false;
                    return;
                }
                request = std::move(m_detailsRequests.front());
                m_detailsRequests.erase(m_detailsRequests.begin());
                latest = m_detailsRequests.empty();
            }
            std::lock_guard<std::mutex> loaderLock(m_loaderMutex);
            if (request.setDisabledAddOns)
            {
                std::vector<const char *> names;
                names.reserve(request.disabledAddOns.size());
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
            if (!m_detailsStopping && m_detailsRequests.empty())
            {
                m_details = std::move(details);
                m_detailsReady = true;
            }
        }
    }
    catch (const std::exception & error)
    {
        LOG_ERROR(Frontend, "Game details loading failed: {}", error.what());
    }
    catch (...)
    {
        LOG_ERROR(Frontend, "Game details loading failed with an unknown exception");
    }
    std::lock_guard<std::mutex> lock(m_detailsMutex);
    // An exception terminated the worker; discard unapplied requests rather
    // than replaying operations from a failed loader call in another session.
    m_detailsRequests.clear();
    m_detailsRunning = false;
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
    if (!info || m_stop)
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
    if (!m_stop)
    {
        m_found.push_back(std::move(found));
    }
}
