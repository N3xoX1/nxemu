#pragma once
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

// What the library scan learns from the file system alone, without opening a
// game: which files there are, and whether one has changed since it was read.

struct LibraryFile
{
    std::filesystem::path path;
    std::string signature; // changes when the file's size or modification time does
};

struct LibraryListing
{
    std::vector<LibraryFile> files; // ordered by path
    // False when the directory or one of its files could not be read. Nothing
    // may be concluded from the files an incomplete listing lacks.
    bool complete = false;
    bool missing = false; // the directory does not exist; the listing is complete and empty
};

namespace LibraryFiles
{
// A file the browser can show as a game.
bool IsGame(const std::filesystem::path & path);
// A file that can hold a game, an update or add-on content.
bool IsContainer(const std::filesystem::path & path);

// One filesystem walk for both the flat game list and recursive add-on index.
// Both are sorted and share conservative completeness status.
struct GameContentListings
{
    LibraryListing games;
    LibraryListing containers;
    LibraryListing modifications; // Title ID directories; files are read live by the loader
};
GameContentListings ListGameContent(const std::filesystem::path & directory, const std::atomic<bool> * cancelled = nullptr);

// The game files directly in a game directory.
LibraryListing ListGames(const std::filesystem::path & directory);
// The containers anywhere below an add-on directory.
LibraryListing ListContainers(const std::filesystem::path & directory);
// Every file below a directory of installed content.
LibraryListing ListAll(const std::filesystem::path & directory);

// The size in bytes of the file a signature was made for.
uint64_t SizeOf(const std::string & signature);

// Identifies the listed files and their state: any file added, removed or
// changed gives another value.
std::string Fingerprint(const LibraryListing & listing);
} // namespace LibraryFiles
