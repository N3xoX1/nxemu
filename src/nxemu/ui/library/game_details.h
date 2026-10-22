#pragma once
#include <cstdint>
#include <nxemu-module-spec/base.h>
#include <string>
#include <vector>

nxinterface ISystemloader;

// One piece of a game's add-on content.
struct GameAddOnItem
{
    uint64_t titleId = 0;
    uint32_t version = 0;
    uint64_t size = 0; // of its data, in bytes
    bool operator==(const GameAddOnItem &) const = default;
};

// Something the loader can apply to a game, or leave out.
struct GameAddOn
{
    enum class Kind
    {
        Update,
        AddOnContent, // all of the game's add-on content: the loader takes it or leaves it as a whole
        Mod,
    };
    Kind kind = Kind::Mod;
    std::string key;     // what the loader's list of disabled add-ons calls it
    std::string name;
    std::string version; // for a mod, what it changes; for add-on content, its numbers
    bool enabled = false;
    std::vector<GameAddOnItem> items; // what add-on content is made of
    bool operator==(const GameAddOn &) const = default;
};

// A setting that has another value for one game than for the others.
struct GameOverride
{
    std::string key;
    std::string name;
    std::string value;
    bool user = false;
    bool effective = true; // enabled and not superseded by a User value
    bool enabled = true;
    bool operator==(const GameOverride &) const = default;
};

// What only opening a game's file tells about it.
struct GameDetails
{
    std::string path;
    uint64_t programId = 0; // 0 when the file could not be read
    std::string version;    // with the enabled update applied
    std::vector<GameAddOn> addOns;
    std::vector<GameOverride> overrides;
    // The game's directories in the emulator's data; the first two are empty
    // until the game has written something there.
    std::string saveDirectory;
    std::string cacheDirectory;
    std::string modDirectory;
};

namespace GameDetailsReader
{
std::string SaveDirectory(uint64_t programId);
GameDetails Read(ISystemloader & loader, const std::string & path);
// The keys to hand the loader as disabled once the add-on at the index is
// switched. Only one update applies at a time: enabling one disables the rest.
std::vector<std::string> DisabledAfterToggle(const GameDetails & details, size_t index);
} // namespace GameDetailsReader
