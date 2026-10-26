#pragma once

#include "emulation_session.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class UiLibrary
{
public:
    struct File
    {
        std::string path;
        uint64_t size = 0;
        int64_t modified = 0;
    };

    struct Game
    {
        std::string path;
        uint64_t programId = 0;
        std::string title;
        std::string publisher;
        std::string version;
        std::vector<uint8_t> icon;
    };

    struct Listener
    {
        std::function<void(Game game)> found;
        std::function<void(uint32_t started, uint32_t total)> progress;
        std::function<void(bool complete)> done;
    };
    using Properties = EmulationSession::GameProperties;
    using AddOn = EmulationSession::GameAddOn;

    ~UiLibrary();

    void Scan(std::vector<File> files, Listener listener);
    static void ReadProperties(std::string path, uint64_t programId, std::function<void(Properties)> receiver);
    void Cancel();

private:
    std::shared_ptr<std::atomic_bool> m_cancelled;
};
