#pragma once
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

    void StartScan(ISystemModules & modules);
    void StopScan();
    bool Scanning() const { return m_scanning; }
    bool TakeFound(std::vector<FoundGame> & games);

private:
    GameLibrary(const GameLibrary &) = delete;
    GameLibrary & operator=(const GameLibrary &) = delete;

    void Scan(ISystemModules & modules, std::vector<std::string> directories);

    // IGameListScanCallback
    bool ScanStopRequested() override;
    void GameFound(const char * path, IRomInfo * romInfo, bool changed) override;

    std::vector<std::string> m_directories;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_scanning{false};
    std::mutex m_foundMutex;
    std::vector<FoundGame> m_found;
};
