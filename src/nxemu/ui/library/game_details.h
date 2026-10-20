#pragma once
#include <cstdint>
#include <nxemu-module-spec/base.h>
#include <string>
#include <vector>

nxinterface ISystemloader;

struct GameAddOn
{
    enum class Kind
    {
        Update,
        AddOnContent,
        Mod,
    };
    Kind kind = Kind::Mod;
    std::string key;
    std::string name;
    std::string version;
    bool enabled = false;
};

struct GameDetails
{
    std::string path;
    uint64_t programId = 0;
    std::string version;
    std::vector<GameAddOn> addOns;
    std::string saveDirectory;
    std::string cacheDirectory;
    std::string modDirectory;
};

namespace GameDetailsReader
{
std::string SaveDirectory(uint64_t programId);
GameDetails Read(ISystemloader & loader, const std::string & path);
std::vector<std::string> DisabledAfterToggle(const GameDetails & details, size_t index);
} // namespace GameDetailsReader
