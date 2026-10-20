#pragma once
#include "game_details.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <nxemu-module-spec/system_loader.h>
#include <string>
#include <thread>
#include <vector>

nxinterface ISystemModules;

struct GameEntry
{
    std::string path;
    uint64_t programId = 0;
    std::string title;
    std::string version;
    std::string publisher;
    std::string icon;
    uint64_t size = 0;
};

struct FoundGame
{
    GameEntry game;
    std::vector<uint8_t> icon;
};

class GameLibrary : public IGameListScanCallback
{
public:
    GameLibrary();
    ~GameLibrary();

    const std::vector<std::string> & Directories() const { return m_directories; }
    bool AddDirectory(const std::string & directory);
    bool RemoveDirectory(const std::string & directory);
    bool ScanSubfolders(const std::string & directory) const;
    void SetScanSubfolders(const std::string & directory, bool scanSubfolders);

    const std::vector<std::string> & RecentFiles() const;
    void AddRecentFile(const std::string & path);

    void StartScan(ISystemModules & modules);
    void StopScan();
    bool Scanning() const { return m_scanning; }
    bool ScanCompleted() const { return m_scanCompleted; }
    bool ReadingGames() const { return m_scanning && m_filesTotal != 0; }
    uint32_t FilesStarted() const { return m_filesStarted; }
    uint32_t FilesTotal() const { return m_filesTotal; }
    bool TakeFound(std::vector<FoundGame> & games);

    void RequestDetails(ISystemModules & modules, const std::string & path);
    void RequestDetails(ISystemModules & modules, const std::string & path, uint64_t programId, std::vector<std::string> disabledAddOns);
    bool TakeDetails(GameDetails & details);
    void StopDetails();

private:
    GameLibrary(const GameLibrary &) = delete;
    GameLibrary & operator=(const GameLibrary &) = delete;

    void Scan(ISystemModules & modules, std::vector<std::string> directories, std::vector<std::string> recursive);

    // IGameListScanCallback
    bool ScanStopRequested() override;
    void GameFound(const char * path, IRomInfo * romInfo, bool changed) override;
    void ScanProgress(uint32_t filesStarted, uint32_t filesTotal) override;

    struct DetailsRequest
    {
        std::string path;
        bool setDisabledAddOns = false;
        uint64_t programId = 0;
        std::vector<std::string> disabledAddOns;
    };
    void QueueDetails(ISystemModules & modules, DetailsRequest request);
    void ReadDetails(ISystemModules & modules);

    std::vector<std::string> m_directories;
    std::vector<std::string> m_recursive;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_scanCompleted{false};
    std::atomic<uint32_t> m_filesStarted{0};
    std::atomic<uint32_t> m_filesTotal{0};
    std::mutex m_foundMutex;
    std::vector<FoundGame> m_found;
    std::mutex m_loaderMutex;
    std::thread m_detailsThread;
    std::mutex m_detailsMutex;
    std::vector<DetailsRequest> m_detailsRequests;
    bool m_detailsRunning = false;
    bool m_detailsStopping = false;
    GameDetails m_details;
    bool m_detailsReady = false;
};
