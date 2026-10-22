#pragma once
#include "game_details.h"
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <nxemu-module-spec/base.h>
#include <string>
#include <thread>
#include <vector>

nxinterface ISystemModules;
class GameLibraryCache;

struct GameEntry
{
    std::string path;
    uint64_t programId = 0; // what the emulated console knows the game by
    std::string title;
    std::string version;   // empty when the file does not say
    std::string publisher; // empty when the file does not say
    std::string icon;      // path of an image file, empty when the game has none
    uint64_t size = 0;     // of the file, in bytes
    bool operator==(const GameEntry &) const = default;
};

// The user's game directories and recent files, and the background scan that
// turns the directories into a list of games.
class GameLibrary
{
public:
    static constexpr size_t MaxRecentFiles = 10;

    GameLibrary();
    ~GameLibrary();

    const std::vector<std::string> & Directories() const { return m_directories; }
    const std::vector<std::string> & RecentFiles() const { return m_recentFiles; }
    // The games the previous scans found in the directories, to show until
    // the running scan has checked them.
    std::vector<GameEntry> CachedGames() const;
    bool AddDirectory(const std::string & directory);
    bool RemoveDirectory(const std::string & directory);
    bool ScanSubfolders(const std::string & directory) const;
    void SetScanSubfolders(const std::string & directory, bool scanSubfolders);
    void AddRecentFile(const std::string & path);

    // Starts scanning the directories with the given modules' loader, replacing
    // any scan in progress. The modules must stay valid until StopScan().
    void StartScan(ISystemModules & modules);
    // Stops using the modules: ends the scan and answers the pending requests
    // for details. Returns true when it interrupted a scan that had not finished.
    bool StopScan();
    // Poll before releasing modules so background I/O never holds up the UI.
    bool UsingModules();
    // Returns true and fills games once, when a scan has finished.
    bool TakeScanResult(std::vector<GameEntry> & games);
    bool Scanning() const { return m_scanning; }
    // True while a scan is opening game files, which is what takes time.
    bool ReadingGames() const { return m_scanning && m_readingGames; }
    struct Progress
    {
        uint32_t started = 0; // game files the scan has got to, the current one included
        uint32_t total = 0;
    };
    Progress ScanProgress() const { return {m_filesStarted, m_filesToScan}; }

    // Reads in the background what the details of a game take opening its
    // file, once a running scan is done with the loader. With disabledAddOns,
    // first makes them the add-ons the loader leaves out of the program.
    // The modules must stay valid until StopScan().
    void RequestDetails(ISystemModules & modules, const std::string & path);
    void RequestDetails(ISystemModules & modules, const std::string & path, uint64_t programId, std::vector<std::string> disabledAddOns);
    // Returns true and fills details once, with the answer to the latest request.
    bool TakeDetails(GameDetails & details);

private:
    GameLibrary(const GameLibrary &) = delete;
    GameLibrary & operator=(const GameLibrary &) = delete;

    struct DetailsRequest
    {
        std::string path;
        bool setDisabledAddOns = false;
        uint64_t programId = 0;
        std::vector<std::string> disabledAddOns;
    };

    void Scan(ISystemModules & modules, const std::vector<std::string> & directories);
    void QueueDetails(ISystemModules & modules, DetailsRequest request);
    void ReadDetails(ISystemModules & modules);

    std::vector<std::string> m_directories;
    std::vector<std::string> m_recursive;
    std::vector<std::string> m_recentFiles;
    std::unique_ptr<GameLibraryCache> m_cache;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_readingGames{false};
    std::atomic<uint32_t> m_filesToScan{0};
    std::atomic<uint32_t> m_filesStarted{0};
    std::mutex m_resultMutex;
    std::vector<GameEntry> m_result;
    bool m_resultReady = false;
    std::mutex m_loaderMutex; // the scan and the details take the loader in turn
    std::thread m_detailsThread;
    std::mutex m_detailsMutex;
    std::deque<DetailsRequest> m_detailsRequests;
    bool m_detailsRunning = false;
    GameDetails m_details;
    bool m_detailsReady = false;
};
