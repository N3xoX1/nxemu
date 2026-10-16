#include "game_library_cache.h"
#include "game_cache.h"
#include "library_files.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <yuzu_common/fs/fs_util.h>

namespace
{
void Check(bool condition, const char * message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void Write(const std::filesystem::path & path, std::string_view contents)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    Check(static_cast<bool>(file), "fixture write failed");
}

std::string Utf8(const std::filesystem::path & path)
{
    return Common::FS::PathToUTF8String(path);
}

struct TemporaryDirectory
{
    TemporaryDirectory()
    {
        path = std::filesystem::temp_directory_path() /
               ("nxemu-library-cache-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
    std::filesystem::path path;
};

void TestListings(const std::filesystem::path & root)
{
    const std::filesystem::path games = root / "games";
    std::filesystem::create_directories(games / "below");
    Write(games / "zelda.dnsp", "base game");
    Write(games / "homebrew.NRO", "homebrew");
    Write(games / "notes.txt", "not a game");
    Write(games / "below" / "update.nca", "update");
    Write(games / "below" / "tool.nro", "not a container");

    const LibraryListing direct = LibraryFiles::ListGames(games);
    Check(direct.complete && !direct.missing && direct.files.size() == 2, "games directly in the directory");
    Check(direct.files[0].path == games / "homebrew.NRO" && direct.files[1].path == games / "zelda.dnsp", "listing order");
    const LibraryListing containers = LibraryFiles::ListContainers(games);
    Check(containers.complete && containers.files.size() == 2, "containers below the directory");
    Check(LibraryFiles::ListAll(games).files.size() == 5, "every file below the directory");

    const LibraryListing absent = LibraryFiles::ListGames(root / "absent");
    Check(absent.complete && absent.missing && absent.files.empty(), "missing directory");
    const LibraryListing notDirectory = LibraryFiles::ListGames(games / "notes.txt");
    Check(!notDirectory.complete, "a file is not a listable directory");

    const std::string fingerprint = LibraryFiles::Fingerprint(direct);
    Check(LibraryFiles::Fingerprint(LibraryFiles::ListGames(games)) == fingerprint, "unchanged directory, same fingerprint");
    Write(games / "notes.txt", "still not a game");
    Check(LibraryFiles::Fingerprint(LibraryFiles::ListGames(games)) == fingerprint, "unrelated file, same fingerprint");
    Write(games / "zelda.dnsp", "a longer base game");
    const LibraryListing changed = LibraryFiles::ListGames(games);
    Check(changed.files[1].signature != direct.files[1].signature && changed.files[0].signature == direct.files[0].signature,
          "only the changed file has another signature");
    Check(LibraryFiles::Fingerprint(changed) != fingerprint, "changed file, another fingerprint");
    std::filesystem::remove(games / "homebrew.NRO");
    Check(LibraryFiles::Fingerprint(LibraryFiles::ListGames(games)) != LibraryFiles::Fingerprint(changed), "removed file, another fingerprint");
}

void TestCache(const std::filesystem::path & root)
{
    const std::filesystem::path directory = root / "cache" / "game-library";
    const std::vector<uint8_t> image = {0, 1, 128, 255};

    GameLibraryCache cache(directory);
    Check(!cache.Load(), "no index yet");
    Check(cache.Save() && !std::filesystem::exists(directory), "nothing to write");

    const std::string icon = cache.StoreIcon(image);
    Check(!icon.empty() && cache.StoreIcon(image) == icon, "one file per distinct icon");
    Check(cache.StoreIcon({}).empty(), "no image, no icon");
    const std::string unused = cache.StoreIcon({9, 9, 9});

    CachedGame game;
    game.game.path = "/games/zelda.dnsp";
    game.game.title = "The Legend of Zelda";
    game.game.version = "1.9.0";
    game.game.publisher = "Nintendo";
    game.game.icon = icon;
    game.baseVersion = "1.0.0";
    game.fileSignature = "10:20";
    game.game.size = 10; // what the signature says
    game.dependencySignature = "abc";
    cache.PutGame(game);
    CachedGame plain = game;
    plain.game.path = "/games/homebrew.nro";
    plain.game.icon.clear();
    cache.PutGame(plain);
    cache.PutContainer("/games/zelda.dnsp", {"10:20", {0x0100000000010000ull}});
    cache.PutContainer("/addons/update.dnsp", {"5:6", {0x0100000000010000ull, 0xFFFFFFFFFFFFE000ull}});
    cache.PutRejection("/games/update.dnsp", {"7:8", "common"});
    cache.PutRejection(plain.game.path, {"1:2", "common"});
    Check(!cache.FindGame(plain.game.path), "a rejected file is no longer a game");
    cache.PutGame(plain);
    Check(cache.FindGame(plain.game.path) && !cache.FindRejection(plain.game.path), "a game is no longer rejected");
    Check(cache.Save(), "save");
    Check(std::filesystem::exists(Common::FS::ToU8String(icon)) && !std::filesystem::exists(Common::FS::ToU8String(unused)),
          "icons no game uses are removed");

    const std::filesystem::path index = directory / "index.json";
    const auto written = std::filesystem::last_write_time(index);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(cache.Save() && std::filesystem::last_write_time(index) == written, "an unchanged cache is not rewritten");

    GameLibraryCache reloaded(directory);
    Check(reloaded.Load(), "reload");
    const std::optional<CachedGame> found = reloaded.FindGame(game.game.path);
    Check(found && found->game == game.game && found->baseVersion == "1.0.0" && found->fileSignature == "10:20" &&
          found->dependencySignature == "abc", "game round trip");
    Check(reloaded.FindGame(plain.game.path) && reloaded.Games().size() == 2, "game without icon");
    const std::optional<CachedContainer> container = reloaded.FindContainer("/addons/update.dnsp");
    Check(container && container->fileSignature == "5:6" && container->applications.size() == 2 &&
          container->applications[1] == 0xFFFFFFFFFFFFE000ull, "container round trip");
    Check(!reloaded.FindGame("/games/other.dnsp") && !reloaded.FindContainer("/games/other.dnsp"), "unknown files");
    const std::optional<CachedRejection> rejection = reloaded.FindRejection("/games/update.dnsp");
    Check(rejection && rejection->fileSignature == "7:8" && rejection->dependencySignature == "common", "rejection round trip");
    reloaded.PutRejection("/addons/dlc.dnsp", {"3:4", "common"});

    reloaded.Prune([](const std::string & path) { return path.starts_with("/games/"); });
    Check(reloaded.FindGame(game.game.path) && !reloaded.FindContainer("/addons/update.dnsp") && reloaded.FindRejection("/games/update.dnsp") &&
          !reloaded.FindRejection("/addons/dlc.dnsp"), "prune");
    Check(reloaded.Save(), "save after prune");

    // A game whose icon file is gone is read again rather than shown without.
    std::filesystem::remove(Common::FS::ToU8String(icon));
    GameLibraryCache withoutIcon(directory);
    Check(withoutIcon.Load() && !withoutIcon.FindGame(game.game.path) && withoutIcon.FindGame(plain.game.path), "missing icon file");

    Write(index, "{ \"schema\": 1, \"games\": [], \"containers\": [] }");
    Check(!GameLibraryCache(directory).Load(), "index of another version");
    Write(index, "corrupt");
    GameLibraryCache corrupt(directory);
    Check(!corrupt.Load() && corrupt.Games().empty(), "damaged index");
}
} // namespace

int main()
{
    TemporaryDirectory temporary;
    TestListings(temporary.path);
    TestCache(temporary.path);
    const auto shaders = temporary.path / "shader";
    const auto title = GameCache::Directory(shaders, 0x0100000000010000ull);
    const auto other = GameCache::Directory(shaders, 0x0100000000020000ull);
    const auto saves = temporary.path / "saves";
    std::filesystem::create_directories(title / "vulkan");
    std::filesystem::create_directories(other);
    std::filesystem::create_directories(saves);
    Write(title / "vulkan/pipelines.bin", "cache");
    Write(other / "pipelines.bin", "other game");
    Write(saves / "save.bin", "progress");
    std::string error;
    Check(GameCache::Clear(shaders, 0x0100000000010000ull, error), "clear the selected game's shader cache");
    Check(!std::filesystem::exists(title) && std::filesystem::exists(other / "pipelines.bin") &&
          std::filesystem::exists(saves / "save.bin"), "cache clear preserves other games and saves");
    Check(GameCache::Clear(shaders, 0x0100000000010000ull, error), "clearing an absent cache is harmless");
    Check(!GameCache::Clear(shaders, 0, error), "unknown title must not clear the shader root");
    std::error_code ec;
    std::filesystem::create_directory_symlink(saves, title, ec);
    if (!ec)
    {
        Check(!GameCache::Clear(shaders, 0x0100000000010000ull, error), "refuse symlinked cache title");
        Check(std::filesystem::exists(saves / "save.bin"), "symlink target must survive");
        std::filesystem::remove(title);
        std::filesystem::create_directories(title);
        std::filesystem::create_directory_symlink(saves, title / "nested", ec);
        if (!ec)
        {
            Check(GameCache::Clear(shaders, 0x0100000000010000ull, error), "clear nested links without following them");
            Check(std::filesystem::exists(saves / "save.bin"), "nested link target must survive");
        }
    }

    return 0;
}
