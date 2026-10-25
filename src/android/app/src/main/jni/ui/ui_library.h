#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class UiLibrary
{
public:
    struct Game
    {
        size_t index = 0;
        bool readable = false;
        std::string title;
        std::vector<uint8_t> icon;
    };
    using Receiver = std::function<void(const Game * game)>;

    ~UiLibrary();

    void Read(std::vector<std::string> paths, Receiver receiver);
    void Cancel();

private:
    std::shared_ptr<std::atomic_bool> m_cancelled;
};
