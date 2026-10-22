#pragma once
#include "game_library.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// What a scan read from a game file, and what it was read against.
struct CachedGame
{
    GameEntry game;
    // The version of the game's own control data: what game.version returns
    // to when no update applies any more.
    std::string baseVersion;
    // LibraryFile::signature of the game file. While it matches, everything
    // but the version is still right.
    std::string fileSignature;
    // Identifies all that decides which update applies. While it matches too,
    // so is the version. Empty when that could not be established.
    std::string dependencySignature;
};

// A file a scan opened and found to hold no game: an update or add-on
// content, which the library does not show.
struct CachedRejection
{
    std::string fileSignature;
    // What the loader had to read it with: GameLibrary's common dependencies.
    std::string dependencySignature;
};

// What a scan read from a file that can hold updates or add-on content.
struct CachedContainer
{
    std::string fileSignature;
    std::vector<uint64_t> applications; // ISystemloader::GetContainerApplications
};

// Keeps what the library scan reads from game files between runs, so that an
// unchanged library is shown without opening any of them.
//
// It is a directory: "index.json" describes the games, and "icons" holds one
// image file per distinct icon, named after its contents. Every method may be
// called from any thread.
class GameLibraryCache
{
public:
    explicit GameLibraryCache(std::filesystem::path directory);

    // Reads the index. An index that is missing, damaged or of another
    // version leaves the cache empty, as do games whose icon file is gone.
    bool Load();
    // Writes the index if anything changed since it was read or written, and
    // removes the icons no game uses any more.
    bool Save(bool completeContentScan = true);

    std::optional<CachedGame> FindGame(const std::string & path) const;
    std::optional<CachedContainer> FindContainer(const std::string & path) const;
    std::optional<CachedRejection> FindRejection(const std::string & path) const;
    std::vector<GameEntry> Games() const;

    // The file is a game, and no longer rejected.
    void PutGame(CachedGame game);
    void PutRejection(const std::string & path, CachedRejection rejection);
    void PutContainer(const std::string & path, CachedContainer container);
    // Forgets the files, whatever is known of them, that keep() rejects.
    void Prune(const std::function<bool(const std::string & path)> & keep);

    // Stores an encoded image and returns the path of its file, which is what
    // GameEntry::icon holds. Returns an empty string when it cannot.
    std::string StoreIcon(const std::vector<uint8_t> & image);

private:
    GameLibraryCache(const GameLibraryCache &) = delete;
    GameLibraryCache & operator=(const GameLibraryCache &) = delete;

    std::filesystem::path IconDirectory() const { return m_directory / "icons"; }
    void RemoveUnusedIcons() const;

    const std::filesystem::path m_directory;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, CachedGame> m_games;
    std::unordered_map<std::string, CachedContainer> m_containers;
    std::unordered_map<std::string, CachedRejection> m_rejections;
    bool m_dirty = false;
    // An independent cache stores Title ID associations, so editing one
    // game title or icon does not rewrite the content index.
    bool m_containersDirty = false;
};
