#include "game_library.h"
#include "game_library_cache.h"
#include "library_files.h"
#include "rom_properties.h"
#include "ui_preferences.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <common/json.h>
#include <common/json_util.h>
#include <common/sha256.h>
#include <filesystem>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/system_loader.h>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/logging/log.h>

namespace
{
bool IsValid(IRomInfo * info)
{
    return info != nullptr && info->GetFileType() != LoaderFileType::Unknown && info->GetFileType() != LoaderFileType::Error;
}

bool ReadGame(ISystemloader & loader, GameLibraryCache & cache, const std::string & path, CachedGame & cached)
{
    IRomInfoPtr info(loader.RomInfo(path.c_str(), 0, 0));
    if (!IsValid(info.Get()))
    {
        return false;
    }
    GameEntry & game = cached.game;
    game.path = path;
    info->ReadProgramId(game.programId);
    game.title = RomProperties::Title(*info);
    if (game.title.empty())
    {
        game.title = Common::FS::PathToUTF8String(std::filesystem::path{Common::FS::ToU8String(path)}.filename());
    }
    RomProperties::Release(*info, game.version, game.publisher, cached.baseVersion);
    game.icon = cache.StoreIcon(RomProperties::Icon(*info));
    return true;
}

// Reads again the one property that files other than the game's decide.
bool RefreshVersion(ISystemloader & loader, CachedGame & cached)
{
    IRomInfoPtr info(loader.RomInfo(cached.game.path.c_str(), 0, 0));
    if (!IsValid(info.Get()))
    {
        return false;
    }
    const std::string update = RomProperties::ActiveUpdateVersion(*info);
    cached.game.version = update.empty() ? cached.baseVersion : update;
    return true;
}

std::vector<std::string> AddOnDirectories(const JsonValue & loaderSettings)
{
    std::vector<std::string> directories;
    const JsonValue list = JsonGetNestedValue(loaderSettings, "AddOnDirectories");
    for (uint32_t i = 0; list.isArray() && i < list.size(); i++)
    {
        if (list[i].isString() && !list[i].asString().empty())
        {
            directories.push_back(list[i].asString());
        }
    }
    return directories;
}

// Installed content can affect every game. Loader preferences and another
// game's disabled add-ons must not invalidate the whole library.
// Empty when the installed content could not be listed reliably.
std::string CommonDependencies()
{
    std::string dependencies = "game-library-dependencies-v4";
    const std::filesystem::path nand = Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir);
    const std::filesystem::path sdmc = Common::FS::GetYuzuPath(Common::FS::YuzuPath::SDMCDir);
    for (const std::filesystem::path & directory : {
             nand / "system/Contents/registered",
             nand / "user/Contents/registered",
             sdmc / "Nintendo/Contents/registered",
         })
    {
        const LibraryListing installed = LibraryFiles::ListAll(directory);
        if (!installed.complete)
        {
            return {};
        }
        dependencies += '\n' + LibraryFiles::Fingerprint(installed);
    }
    return sha256(std::move(dependencies));
}

std::unordered_map<uint64_t, std::string> DisabledAddOnDependencies(const JsonValue & loaderSettings)
{
    std::unordered_map<uint64_t, std::string> result;
    const JsonValue & disabled = loaderSettings["DisabledAddOns"];
    for (const std::string & key : disabled.GetMemberNames())
    {
        uint64_t application = 0;
        const auto parsed = std::from_chars(key.data(), key.data() + key.size(), application, 16);
        const JsonValue & list = disabled[key];
        if (key.size() != 16 || parsed.ec != std::errc() || parsed.ptr != key.data() + key.size() || !list.isArray()) continue;
        std::vector<std::string> names;
        for (uint32_t i = 0; i < list.size(); ++i)
            if (list[i].isString()) names.push_back(list[i].asString());
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        for (const std::string & name : names) result[application] += '\n' + name;
    }
    return result;
}

bool IsInside(const std::string & path, const std::string & directory)
{
    const std::filesystem::path relative =
        std::filesystem::path{Common::FS::ToU8String(path)}.lexically_relative(std::filesystem::path{Common::FS::ToU8String(directory)});
    return !relative.empty() && *relative.begin() != "..";
}

void SortByTitle(std::vector<GameEntry> & games)
{
    const auto folded = [](const std::string & text) {
        std::string result = text;
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return result;
    };
    std::sort(games.begin(), games.end(), [&folded](const GameEntry & left, const GameEntry & right) {
        return std::forward_as_tuple(folded(left.title), left.path) < std::forward_as_tuple(folded(right.title), right.path);
    });
}

// One scan's view of the files that can hold content for a game, and of
// which applications each of them holds content for.
class Containers
{
public:
    Containers(ISystemloader & loader, GameLibraryCache & cache, std::atomic<bool> & reading) :
        m_loader(loader),
        m_cache(cache),
        m_reading(reading)
    {
    }

    // The applications a file holds content for; none for a file that is not
    // a container. Opens the file only if it changed since it was last read.
    const std::vector<uint64_t> & Applications(const LibraryFile & file)
    {
        const std::string path = Common::FS::PathToUTF8String(file.path);
        const auto known = m_applications.find(path);
        if (known != m_applications.end())
        {
            return known->second;
        }
        std::vector<uint64_t> & applications = m_applications[path];
        if (!LibraryFiles::IsContainer(file.path))
        {
            return applications;
        }
        if (std::optional<CachedContainer> cached = m_cache.FindContainer(path); cached && cached->fileSignature == file.signature)
        {
            applications = std::move(cached->applications);
            return applications;
        }
        m_reading = true;
        ++m_reads;
        // The size query parses the container too. Read the usual small list
        // in one call, and retry only for a container with many applications.
        applications.resize(16);
        const uint32_t count = m_loader.GetContainerApplications(path.c_str(), applications.data(), static_cast<uint32_t>(applications.size()));
        if (count > applications.size())
        {
            applications.resize(count);
            applications.resize(m_loader.GetContainerApplications(path.c_str(), applications.data(), count));
        }
        else applications.resize(count);
        if (!applications.empty())
        {
            // A file that does not read is tried again: what it lacked, keys
            // for one, may have come since.
            m_cache.PutContainer(path, {file.signature, applications});
        }
        return applications;
    }

    size_t Reads() const { return m_reads; }

private:
    ISystemloader & m_loader;
    GameLibraryCache & m_cache;
    std::atomic<bool> & m_reading;
    size_t m_reads = 0;
    std::unordered_map<std::string, std::vector<uint64_t>> m_applications;
};
} // namespace

GameLibrary::GameLibrary() :
    m_directories(UiPreferences::GetStringList(UiPreferences::GameDirectories)),
    m_recentFiles(UiPreferences::GetStringList(UiPreferences::RecentFiles)),
    m_cache(std::make_unique<GameLibraryCache>(Common::FS::GetYuzuPath(Common::FS::YuzuPath::YuzuDir) /
                                               "game_library"))
{
    m_cache->Load();
    // The single file that held the cache before it became a directory.
    std::error_code error;
    std::filesystem::remove(Common::FS::GetYuzuPath(Common::FS::YuzuPath::CacheDir) / "game-library-v1.cache", error);
}

GameLibrary::~GameLibrary()
{
    StopScan();
}

std::vector<GameEntry> GameLibrary::CachedGames() const
{
    std::vector<GameEntry> games = m_cache->Games();
    // Whether the files are still there is for the scan to say: asking here
    // would hold the window up for as long as a drive takes to answer.
    std::erase_if(games, [this](const GameEntry & game) {
        return std::none_of(m_directories.begin(), m_directories.end(),
                            [&game](const std::string & directory) { return IsInside(game.path, directory); });
    });
    SortByTitle(games);
    return games;
}

bool GameLibrary::AddDirectory(const std::string & directory)
{
    if (directory.empty() || std::find(m_directories.begin(), m_directories.end(), directory) != m_directories.end())
    {
        return false;
    }
    m_directories.push_back(directory);
    UiPreferences::SetStringList(UiPreferences::GameDirectories, m_directories);
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
    UiPreferences::SetStringList(UiPreferences::GameDirectories, m_directories);
    return true;
}

void GameLibrary::AddRecentFile(const std::string & path)
{
    m_recentFiles.erase(std::remove(m_recentFiles.begin(), m_recentFiles.end(), path), m_recentFiles.end());
    m_recentFiles.insert(m_recentFiles.begin(), path);
    if (m_recentFiles.size() > MaxRecentFiles)
    {
        m_recentFiles.resize(MaxRecentFiles);
    }
    UiPreferences::SetStringList(UiPreferences::RecentFiles, m_recentFiles);
}

void GameLibrary::StartScan(ISystemModules & modules)
{
    StopScan();
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_result.clear();
        m_resultReady = false;
    }
    m_stop = false;
    m_readingGames = false;
    m_filesToScan = 0;
    m_filesStarted = 0;
    m_scanning = true;
    m_thread = std::thread([this, &modules, directories = m_directories] { Scan(modules, directories); });
}

bool GameLibrary::UsingModules()
{
    std::lock_guard<std::mutex> lock(m_detailsMutex);
    return m_scanning || m_detailsRunning;
}

bool GameLibrary::StopScan()
{
    m_stop = true;
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    if (m_detailsThread.joinable())
    {
        m_detailsThread.join();
    }
    return m_scanning.exchange(false);
}

void GameLibrary::RequestDetails(ISystemModules & modules, const std::string & path)
{
    QueueDetails(modules, {path});
}

void GameLibrary::RequestDetails(ISystemModules & modules, const std::string & path, uint64_t programId, std::vector<std::string> disabledAddOns)
{
    QueueDetails(modules, {path, true, programId, std::move(disabledAddOns)});
}

void GameLibrary::QueueDetails(ISystemModules & modules, DetailsRequest request)
{
    {
        std::lock_guard<std::mutex> lock(m_detailsMutex);
        m_detailsRequests.push_back(std::move(request));
        m_detailsReady = false;
        if (m_detailsRunning)
        {
            return;
        }
        m_detailsRunning = true;
    }
    if (m_detailsThread.joinable())
    {
        m_detailsThread.join(); // it has answered everything, and is leaving
    }
    m_detailsThread = std::thread([this, &modules] { ReadDetails(modules); });
}

void GameLibrary::ReadDetails(ISystemModules & modules)
{
    std::lock_guard<std::mutex> loaderLock(m_loaderMutex);
    ISystemloader & loader = modules.Systemloader();
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
            m_detailsRequests.pop_front();
            latest = m_detailsRequests.empty();
        }
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
            continue; // a game the user has already left
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

bool GameLibrary::TakeDetails(GameDetails & details)
{
    std::lock_guard<std::mutex> lock(m_detailsMutex);
    if (!m_detailsReady)
    {
        return false;
    }
    m_detailsReady = false;
    details = std::move(m_details);
    return true;
}

bool GameLibrary::TakeScanResult(std::vector<GameEntry> & games)
{
    std::lock_guard<std::mutex> lock(m_resultMutex);
    if (!m_resultReady)
    {
        return false;
    }
    m_resultReady = false;
    games = std::move(m_result);
    return true;
}

void GameLibrary::Scan(ISystemModules & modules, const std::vector<std::string> & directories)
{
    std::lock_guard<std::mutex> loaderLock(m_loaderMutex);
    const auto started = std::chrono::steady_clock::now();
    ISystemloader & loader = modules.Systemloader();

    // What the file system says, before any game is opened.
    struct Directory
    {
        std::string path;
        LibraryListing listing;
        std::unordered_map<uint64_t, std::vector<const LibraryFile *>> applications;
    };
    const JsonValue loaderSettings = SettingsStore::GetInstance().GetSettings("nxemu-loader");
    std::string commonDependencies = CommonDependencies();
    const auto disabledAddOns = DisabledAddOnDependencies(loaderSettings);
    std::vector<Directory> gameDirectories;
    std::vector<Directory> addOnDirectories;
    uint32_t fileCount = 0;
    for (const std::string & directory : directories)
    {
        gameDirectories.push_back({directory, LibraryFiles::ListGames(std::filesystem::path{Common::FS::ToU8String(directory)})});
        fileCount += static_cast<uint32_t>(gameDirectories.back().listing.files.size());
    }
    for (const std::string & directory : AddOnDirectories(loaderSettings))
    {
        addOnDirectories.push_back({directory, LibraryFiles::ListContainers(std::filesystem::path{Common::FS::ToU8String(directory)})});
        if (!addOnDirectories.back().listing.complete)
        {
            // An update may be among the files that could not be listed.
            commonDependencies.clear();
        }
    }
    m_filesToScan = fileCount;

    // Identifies what decides the version of one game: the common part, and
    // the other files holding content for the same application, which are the
    // ones the loader looks for updates in. An unrelated file, such as a new
    // game, leaves it unchanged.
    const auto listed = std::chrono::steady_clock::now();
    Containers containers(loader, *m_cache, m_readingGames);
    // Index once by application instead of comparing every game with every
    // other container. Unrelated additions leave existing dependencies intact.
    const auto index = [&](std::vector<Directory> & list) {
        for (Directory & directory : list)
            for (const LibraryFile & file : directory.listing.files)
            {
                if (m_stop) return;
                for (uint64_t application : containers.Applications(file))
                    directory.applications[application].push_back(&file);
            }
    };
    if (!commonDependencies.empty())
    {
        index(gameDirectories);
        index(addOnDirectories);
    }
    const auto dependenciesOf = [&](const LibraryFile & game, const Directory & directory) -> std::string {
        if (commonDependencies.empty() || !directory.listing.complete)
        {
            return {};
        }
        std::string dependencies = commonDependencies;
        const std::vector<uint64_t> applications = containers.Applications(game);
        std::vector<const LibraryFile *> related;
        const auto add = [&](const Directory & source) {
            for (uint64_t application : applications)
            {
                const auto found = source.applications.find(application);
                if (found != source.applications.end()) related.insert(related.end(), found->second.begin(), found->second.end());
            }
        };
        add(directory);
        for (const Directory & addOns : addOnDirectories)
        {
            add(addOns);
        }
        std::sort(related.begin(), related.end(), [](const LibraryFile * left, const LibraryFile * right) { return left->path < right->path; });
        related.erase(std::unique(related.begin(), related.end(), [](const LibraryFile * left, const LibraryFile * right) { return left->path == right->path; }), related.end());
        for (const LibraryFile * other : related)
            if (other->path != game.path) dependencies += '\n' + Common::FS::PathToUTF8String(other->path) + '\n' + other->signature;
        for (uint64_t application : applications)
        {
            const auto disabled = disabledAddOns.find(application);
            if (disabled != disabledAddOns.end()) dependencies += disabled->second;
        }
        return sha256(std::move(dependencies));
    };

    std::vector<GameEntry> games;
    std::unordered_set<std::string> present;
    size_t reused = 0;
    size_t refreshed = 0;
    size_t read = 0;
    size_t skipped = 0;
    for (const Directory & directory : gameDirectories)
    {
        for (const LibraryFile & file : directory.listing.files)
        {
            if (m_stop)
            {
                break;
            }
            m_filesStarted++;
            const std::string path = Common::FS::PathToUTF8String(file.path);
            if (!present.insert(path).second)
            {
                continue;
            }
            std::optional<CachedGame> known = m_cache->FindGame(path);
            if (known && known->fileSignature != file.signature)
            {
                known.reset();
            }
            // A file opened before and found to hold no game, an update for
            // one, is not opened again while nothing it was read with changed.
            if (!known && !commonDependencies.empty())
            {
                const std::optional<CachedRejection> rejected = m_cache->FindRejection(path);
                if (rejected && rejected->fileSignature == file.signature && rejected->dependencySignature == commonDependencies)
                {
                    skipped++;
                    continue;
                }
            }
            // The other files are looked at first: opening the game is then
            // the only slow step left, and often not needed.
            const std::string dependencies = dependenciesOf(file, directory);
            if (known && !dependencies.empty() && known->dependencySignature == dependencies)
            {
                games.push_back(std::move(known->game));
                reused++;
                continue;
            }

            m_readingGames = true;
            CachedGame cached;
            if (known)
            {
                cached = std::move(*known);
                if (!RefreshVersion(loader, cached))
                {
                    // Still shown as it was; the next scan tries again.
                    games.push_back(std::move(cached.game));
                    continue;
                }
                refreshed++;
            }
            else if (ReadGame(loader, *m_cache, path, cached))
            {
                read++;
            }
            else
            {
                // Remembered only when the file itself reads, as a container
                // of something: one that does not read at all, on a drive
                // that failed for one, is tried again by the next scan.
                if (!m_stop && !commonDependencies.empty() && !containers.Applications(file).empty())
                {
                    m_cache->PutRejection(path, {file.signature, commonDependencies});
                }
                continue;
            }
            cached.fileSignature = file.signature;
            cached.game.size = LibraryFiles::SizeOf(file.signature);
            cached.dependencySignature = dependencies;
            games.push_back(cached.game);
            m_cache->PutGame(std::move(cached));
        }
    }
    for (const Directory & directory : addOnDirectories)
    {
        for (const LibraryFile & file : directory.listing.files)
        {
            present.insert(Common::FS::PathToUTF8String(file.path));
        }
    }
    // A directory that could not be listed to the end says nothing of the
    // games it did not give: those known from before stay shown.
    if (!m_stop)
    {
        std::vector<GameEntry> before;
        for (const Directory & directory : gameDirectories)
        {
            if (directory.listing.complete)
            {
                continue;
            }
            if (before.empty())
            {
                before = m_cache->Games();
            }
            for (GameEntry & game : before)
            {
                std::error_code error;
                if (!game.path.empty() && !present.contains(game.path) && IsInside(game.path, directory.path) &&
                    std::filesystem::is_regular_file(std::filesystem::path{Common::FS::ToU8String(game.path)}, error))
                {
                    present.insert(game.path);
                    games.push_back(std::move(game));
                    game.path.clear();
                }
            }
        }
    }

    // What was read is kept even if the scan was interrupted. Files are only
    // forgotten once a finished scan has established that they are gone: not
    // those of a directory that could not be listed or, being on a drive that
    // is not connected, does not exist right now.
    const bool finished = !m_stop;
    if (finished)
    {
        m_cache->Prune([&](const std::string & path) {
            if (present.contains(path))
            {
                return true;
            }
            const auto unknownIn = [&path](const std::vector<Directory> & list) {
                return std::any_of(list.begin(), list.end(), [&path](const Directory & directory) {
                    return (!directory.listing.complete || directory.listing.missing) && IsInside(path, directory.path);
                });
            };
            return unknownIn(gameDirectories) || unknownIn(addOnDirectories);
        });
    }
    if (!m_cache->Save())
    {
        LOG_WARNING(Frontend, "Could not save the game library cache");
    }
    if (!finished)
    {
        return;
    }
    SortByTitle(games);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    LOG_INFO(Frontend, "Game library scan: {} games, {} reused, {} versions refreshed, {} fully read, {} other files skipped in {} ms (listing {} ms, {} containers read)",
             games.size(), reused, refreshed, read, skipped, elapsed.count(),
             std::chrono::duration_cast<std::chrono::milliseconds>(listed - started).count(), containers.Reads());

    std::lock_guard<std::mutex> lock(m_resultMutex);
    m_result = std::move(games);
    m_resultReady = true;
    m_scanning = false;
    m_readingGames = false;
}
