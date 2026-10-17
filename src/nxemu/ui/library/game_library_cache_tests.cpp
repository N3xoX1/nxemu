#include "game_library_cache.h"
#include "game_cache.h"
#include <nxemu-module-spec/game_content_cache_store.h>
#include "library_files.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <iostream>
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

void TestContainerCacheFormat()
{
    JsonValue item(JsonValueType::Object);
    item["path"] = "/updates/title.dnsp";
    item["file"] = "128:9876";
    JsonValue apps(JsonValueType::Array);
    apps.Append(JsonValue("0000000000000029"));
    item["applications"] = apps;
    std::string path, signature;
    std::vector<uint64_t> ids;
    Check(GameContentCacheStore::ReadContainer(item, path, signature, ids) &&
          path == "/updates/title.dnsp" && signature == "128:9876" &&
          ids.size() == 1 && ids.front() == 41, "shared cache format accepts valid title IDs");
    item["applications"][0] = JsonValue("wrong!");
    Check(!GameContentCacheStore::ReadContainer(item, path, signature, ids),
          "shared cache format rejects invalid title IDs");
}

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
    const auto combined = LibraryFiles::ListGameContent(games);
    std::atomic<bool> cancelled{true};
    const auto interrupted = LibraryFiles::ListGameContent(games, &cancelled);
    Check(!interrupted.containers.complete && interrupted.containers.files.empty(),
          "cancelled filesystem traversal cannot look like a complete empty directory");
    Check(combined.games.complete && combined.containers.complete &&
          LibraryFiles::Fingerprint(combined.games) == LibraryFiles::Fingerprint(direct) &&
          LibraryFiles::Fingerprint(combined.containers) == LibraryFiles::Fingerprint(containers),
          "one-pass listing agrees with separate listings");
    Check(LibraryFiles::ListAll(games).files.size() == 5, "every file below the directory");

    const auto absentBoth = LibraryFiles::ListGameContent(root / "absent");
    Check(absentBoth.games.complete && absentBoth.games.missing && absentBoth.games.files.empty() &&
          absentBoth.containers.complete && absentBoth.containers.missing && absentBoth.containers.files.empty(),
          "single-pass missing root is represented consistently");
    Check(absentBoth.modifications.complete && absentBoth.modifications.missing &&
          absentBoth.modifications.files.empty(), "missing root includes mod status");
    const std::filesystem::path modRoot = games / "custom" / "0000000000000001";
    std::filesystem::create_directories(modRoot / "romfs");
    auto mods = LibraryFiles::ListGameContent(games).modifications;
    Check(mods.complete && mods.files.size() == 1, "only the mod root is indexed");
    const std::string firstMod = LibraryFiles::Fingerprint(mods);
    Write(modRoot / "romfs" / "patch.bin", "first");
    mods = LibraryFiles::ListGameContent(games).modifications;
    Check(mods.files.size() == 1 && LibraryFiles::Fingerprint(mods) == firstMod,
          "mod files remain live under the indexed directory");
    const auto secondMod = LibraryFiles::Fingerprint(mods);
    Write(modRoot / "romfs" / "patch.bin", "changed payload");
    mods = LibraryFiles::ListGameContent(games).modifications;
    Check(LibraryFiles::Fingerprint(mods) == secondMod, "mod file changes do not require rebuilding directory lookup");
    std::filesystem::remove_all(modRoot);
    mods = LibraryFiles::ListGameContent(games).modifications;
    Check(mods.files.empty(), "removing a mod tree changes fingerprint");
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
    const std::filesystem::path contentIndex = GameContentCacheStore::IndexPath(directory.parent_path());
    Check(std::filesystem::exists(contentIndex), "independent content cache was written");
    GameContentCacheStore::Entries persisted;
    Check(GameContentCacheStore::Read(contentIndex, persisted) && persisted.size() == 2 &&
        persisted.at("/addons/update.dnsp").applications.size() == 2,
        "shared cache round trip includes Title IDs");
    std::ifstream uiInput(index);
    const std::string uiJson((std::istreambuf_iterator<char>(uiInput)), std::istreambuf_iterator<char>());
    uiInput.close();
    JsonValue uiRoot;
    Check(JsonReader().Parse(uiJson.data(), uiJson.data() + uiJson.size(), uiRoot) &&
        uiRoot["schema"].asInt64() == GameContentCacheStore::LibrarySchema &&
        uiRoot["containers"].isNull(), "UI cache no longer owns container index");
    const auto contentWritten = std::filesystem::last_write_time(contentIndex);
    const auto written = std::filesystem::last_write_time(index);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(cache.Save() && std::filesystem::last_write_time(index) == written &&
          std::filesystem::last_write_time(contentIndex) == contentWritten,
          "unchanged UI and content caches are not rewritten");

    // The two indexes should be written independently, not merely stored
    // in different paths while every UI edit rewrites the content metadata.
    cache.PutContainer("/addons/other.dnsp", {"8:9", {0x0100000000010000ull}});
    Check(cache.Save() && std::filesystem::last_write_time(index) == written,
          "content-only update does not rewrite UI game metadata");
    const auto contentOnlyWritten = std::filesystem::last_write_time(contentIndex);
    CachedGame renamed = game;
    renamed.game.title = "Renamed in frontend";
    cache.PutGame(renamed);
    Check(cache.Save() && std::filesystem::last_write_time(contentIndex) == contentOnlyWritten,
          "UI-only update does not rewrite content index");
    cache.PutGame(game);
    Check(cache.Save(), "restore original UI title");

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

    Write(index, "{ \"schema\": 2, \"games\": [], \"rejected\": [] }");
    Check(!GameLibraryCache(directory).Load(), "index of another version");
    Write(index, "corrupt");
    GameLibraryCache corrupt(directory);
    Check(!corrupt.Load() && corrupt.Games().empty(), "damaged index");
}

void TestIndependentCacheRecovery(const std::filesystem::path & root)
{
    const auto directory = root / "recovery" / "game_library";
    const auto uiIndex = directory / "index.json";
    const auto contentIndex = GameContentCacheStore::IndexPath(directory.parent_path());
    GameLibraryCache cache(directory);
    cache.PutContainer("/addons/old.dnsp", {"25:30", {0x0100000000010000ull}});
    Check(cache.Save(), "content cache can be saved before any game is scanned");
    Check(std::filesystem::exists(contentIndex) && !std::filesystem::exists(uiIndex),
          "content cache is independent of the UI cache");
    GameLibraryCache reloaded(directory);
    Check(!reloaded.Load() && reloaded.FindContainer("/addons/old.dnsp"),
          "missing UI index does not discard content cache");
    Write(contentIndex, "broken");
    GameLibraryCache corruptContent(directory);
    Check(!corruptContent.Load() && !corruptContent.FindContainer("/addons/old.dnsp"),
          "corrupt content cache is rejected");
    Check(corruptContent.Save(false), "partial scan cannot overwrite content index");
    std::ifstream input(contentIndex);
    Check(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()) == "broken",
          "partial scan preserved old content cache");
    input.close();
    Check(corruptContent.Save(), "complete scan can repair broken content cache");
    GameContentCacheStore::Entries repaired;
    Check(GameContentCacheStore::Read(contentIndex, repaired) && repaired.empty(),
          "repaired content cache is readable");
}
 } // namespace

int main()
{
    try {
    TemporaryDirectory temporary;
    TestContainerCacheFormat();
    TestListings(temporary.path);
    TestCache(temporary.path);
    TestIndependentCacheRecovery(temporary.path);
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
    } catch (const std::exception & error) { std::cerr << error.what() << std::endl; return 1; }
}
