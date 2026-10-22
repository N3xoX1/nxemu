#include "game_library.h"
#include "game_details.h"
#include "ui_preferences.h"
#include <nxemu-core/settings/settings.h>
#include <common/json.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/modules/loader_module.h>
#include <nxemu-core/modules/module_settings.h>
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-module-spec/game_content_cache_store.h>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/logging/backend.h>
#include <chrono>
#include <array>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool ok, const char * message) { if (!ok) throw std::runtime_error(message); }
void Write(const std::filesystem::path & path, const std::string & text) { std::ofstream file(path); file << text; Check(bool(file), "write fixture"); }
class CountingFile : public IVirtualFile {
    IVirtualFile * file;
    size_t & reads;
public:
    CountingFile(IVirtualFile * value, size_t & count) : file(value), reads(count) {}
    uint64_t GetSize() const override { return file->GetSize(); }
    const char * GetName() const override { return file->GetName(); }
    const char * GetFullPath() const override { return file->GetFullPath(); }
    bool Resize(uint64_t size) override { return file->Resize(size); }
    uint64_t ReadBytes(uint8_t * data, uint64_t size, uint64_t offset) override { ++reads; return file->ReadBytes(data,size,offset); }
    uint64_t WriteBytes(const uint8_t * data, uint64_t size, uint64_t offset) override { return file->WriteBytes(data,size,offset); }
    IVirtualDirectory * ExtractRomFS() override { return file->ExtractRomFS(); }
    IVirtualDirectory * GetContainingDirectory() const override { return file->GetContainingDirectory(); }
    IVirtualFile * Duplicate() override { return new CountingFile(file->Duplicate(),reads); }
    bool Rename(const char * name) override { return file->Rename(name); }
    void Release() override { file->Release(); delete this; }
};
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("nxemu-library-scan-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};
class LoaderProxy : public ISystemloader {
public:
    ISystemloader * real = nullptr;
    size_t reads = 0, containerReads = 0, indexRefreshes = 0;
    std::unordered_map<std::string, size_t> romReadsByPath;
    void GetBackingReadTraffic(StorageReadTraffic * out) const override { if (real) real->GetBackingReadTraffic(out); else *out = {}; }
    void RefreshGameContentIndex() override { ++indexRefreshes; if (real) real->RefreshGameContentIndex(); }
    bool PublishGameContentSnapshot(const GameContentSnapshot & snapshot) override {
        return real ? real->PublishGameContentSnapshot(snapshot) : true;
    }
    bool Initialize() override { if (real) return real->Initialize(); throw std::runtime_error("Unexpected Initialize"); }
    uint32_t GetSupportedGameExtensions(const char ** extensions, uint32_t maxCount) const override { if (real) return real->GetSupportedGameExtensions(extensions, maxCount); throw std::runtime_error("Unexpected GetSupportedGameExtensions"); }
    bool LoadRom(const char * fileName, uint64_t program_id, int32_t program_index, int32_t previous_program_index, ApplicationLaunchType launch_type) override { if (real) return real->LoadRom(fileName, program_id, program_index, previous_program_index, launch_type); throw std::runtime_error("Unexpected LoadRom"); }
    IRomInfo * RomInfo(const char * fileName, uint64_t programId, uint64_t programIndex) override { ++reads; if (real) return real->RomInfo(fileName, programId, programIndex); throw std::runtime_error("Unexpected RomInfo"); }
    IRomInfo * FileRomInfo(IVirtualFile * file, uint64_t programId, uint64_t programIndex) override { if (real) return real->FileRomInfo(file, programId, programIndex); throw std::runtime_error("Unexpected FileRomInfo"); }
    IRomInfo * LoadedRomInfo() override { if (real) return real->LoadedRomInfo(); throw std::runtime_error("Unexpected LoadedRomInfo"); }
    IContentProvider & ContentProvider() override { if (real) return real->ContentProvider(); throw std::runtime_error("Unexpected ContentProvider"); }
    IFilesystem & Filesystem() override { if (real) return real->Filesystem(); throw std::runtime_error("Unexpected Filesystem"); }
    IFileSystemController & FileSystemController() override { if (real) return real->FileSystemController(); throw std::runtime_error("Unexpected FileSystemController"); }
    IVirtualFile * SynthesizeSystemArchive(const uint64_t title_id) override { if (real) return real->SynthesizeSystemArchive(title_id); throw std::runtime_error("Unexpected SynthesizeSystemArchive"); }
    IVirtualFile * CreateMemoryFile(const uint8_t * data, uint64_t size, const char * name) override { if (real) return real->CreateMemoryFile(data, size, name); throw std::runtime_error("Unexpected CreateMemoryFile"); }
    uint32_t GetContentProviderEntriesCount(bool useTitleType, LoaderTitleType titleType, bool useContentRecordType, LoaderContentRecordType contentRecordType, bool useTitleId, unsigned long long titleId) override { if (real) return real->GetContentProviderEntriesCount(useTitleType, titleType, useContentRecordType, contentRecordType, useTitleId, titleId); throw std::runtime_error("Unexpected GetContentProviderEntriesCount"); }
    uint32_t GetContentProviderEntries(bool useTitleType, LoaderTitleType titleType, bool useContentRecordType, LoaderContentRecordType contentRecordType, bool useTitleId, unsigned long long titleId, ContentProviderEntry * entries, uint32_t entryCount) override { if (real) return real->GetContentProviderEntries(useTitleType, titleType, useContentRecordType, contentRecordType, useTitleId, titleId, entries, entryCount); throw std::runtime_error("Unexpected GetContentProviderEntries"); }
    IFileSysNCA * GetContentProviderEntry(uint64_t title_id, LoaderContentRecordType type) override { if (real) return real->GetContentProviderEntry(title_id, type); throw std::runtime_error("Unexpected GetContentProviderEntry"); }
    IFileSysNACP * ReadControlData() override { if (real) return real->ReadControlData(); throw std::runtime_error("Unexpected ReadControlData"); }
    IFileSysNACP * GetPMControlMetadata(uint64_t programID) override { if (real) return real->GetPMControlMetadata(programID); throw std::runtime_error("Unexpected GetPMControlMetadata"); }
    IManualContentProvider & ManualContentProvider() override { if (real) return real->ManualContentProvider(); throw std::runtime_error("Unexpected ManualContentProvider"); }
    uint32_t GetInstalledFirmwareDisplayVersion(char * buffer, uint32_t buffer_size) const override { if (real) return real->GetInstalledFirmwareDisplayVersion(buffer, buffer_size); throw std::runtime_error("Unexpected GetInstalledFirmwareDisplayVersion"); }
    uint32_t GetGameCardFirmwareUpdate(const char * utf8_path, char * versionBuffer, uint32_t versionBufferSize) const override { if (real) return real->GetGameCardFirmwareUpdate(utf8_path, versionBuffer, versionBufferSize); throw std::runtime_error("Unexpected GetGameCardFirmwareUpdate"); }
    FirmwareInstallStatus InstallFirmwarePackage(const char * utf8_path, IFirmwareInstallObserver * observer, char * versionBuffer, uint32_t versionBufferSize) override { if (real) return real->InstallFirmwarePackage(utf8_path, observer, versionBuffer, versionBufferSize); throw std::runtime_error("Unexpected InstallFirmwarePackage"); }
    void SetDisabledAddons(uint64_t program_id, const char * const * names, uint32_t count) override { if (real) return real->SetDisabledAddons(program_id, names, count); throw std::runtime_error("Unexpected SetDisabledAddons"); }
    bool IsAddonDisabled(uint64_t program_id, const char * name) const override { if (real) return real->IsAddonDisabled(program_id, name); throw std::runtime_error("Unexpected IsAddonDisabled"); }
    LoaderResultStatus GetPMControlData(uint64_t program_id, uint8_t * buffer, uint32_t buffer_size, uint32_t & actual_size) override { if (real) return real->GetPMControlData(program_id, buffer, buffer_size, actual_size); throw std::runtime_error("Unexpected GetPMControlData"); }
    uint32_t GetContainerApplications(const char * utf8_path, uint64_t * application_ids, uint32_t max_count) override { ++containerReads; if (real) return real->GetContainerApplications(utf8_path, application_ids, max_count); throw std::runtime_error("Unexpected GetContainerApplications"); }
};
class FakeRom : public IRomInfo {
public:
    uint64_t id;
    explicit FakeRom(uint64_t value) : id(value) {}
    LoaderFileType GetFileType() const override { return LoaderFileType::NSP; }
    LoaderResultStatus ReadProgramId(uint64_t & out_program_id) override { out_program_id = id; return LoaderResultStatus::Success; }
    LoaderResultStatus ReadTitle(char * buffer, uint32_t * bufferSize) override { *bufferSize = 0; return LoaderResultStatus::Success; }
    LoaderResultStatus ReadDisplayVersion(char * buffer, uint32_t * bufferSize) override { *bufferSize = 0; return LoaderResultStatus::Success; }
    LoaderResultStatus ReadIcon(uint8_t * buffer, uint32_t * bufferSize) override { *bufferSize = 0; return LoaderResultStatus::ErrorNoIcon; }
    LoaderResultStatus ReadBanner(uint8_t * buffer, uint32_t * bufferSize) override { throw std::runtime_error("Unexpected ReadBanner"); }
    LoaderResultStatus ReadLogo(uint8_t * buffer, uint32_t * bufferSize) override { throw std::runtime_error("Unexpected ReadLogo"); }
    LoaderResultStatus ReadProgramIds(uint64_t * buffer, uint32_t * count) override { throw std::runtime_error("Unexpected ReadProgramIds"); }
    LoaderResultStatus Load(ISystemModules & modules, GuestProcessLoadParameters * out_parameters) override { throw std::runtime_error("Unexpected Load"); }
    void AddToManualContentProvider(IManualContentProvider & provider) override { throw std::runtime_error("Unexpected AddToManualContentProvider"); }
    void PrepareManualContent() override { throw std::runtime_error("Unexpected PrepareManualContent"); }
    uint32_t GetGamePatches(GamePatchInfo * patches, uint32_t maxCount) override { throw std::runtime_error("Unexpected GetGamePatches"); }
    uint32_t GetAddOnContents(GameAddOnContentInfo * contents, uint32_t maxCount) override { throw std::runtime_error("Unexpected GetAddOnContents"); }
    LoaderResultStatus ReadActiveUpdateVersion(char * buffer, uint32_t * bufferSize) override { *bufferSize = 0; return LoaderResultStatus::Success; }
    IFileSysNACP * GetBaseControlMetadata() override { return nullptr; }
    IFileSysNACP * GetControlMetadata() override { throw std::runtime_error("Unexpected GetControlMetadata"); }
    IVirtualFile * ReadManualRomFS() override { throw std::runtime_error("Unexpected ReadManualRomFS"); }
    void Release() override { delete this; }
};
class FakeLoader : public LoaderProxy {
public:
    bool containerKeysAvailable = true;
    IRomInfo * RomInfo(const char * fileName, uint64_t, uint64_t) override {
        ++reads; ++romReadsByPath[fileName];
        std::ifstream file(fileName); std::string kind; uint64_t id = 0; file >> kind >> id;
        return kind == "game" ? new FakeRom(id) : nullptr;
    }
    uint32_t GetContainerApplications(const char * path, uint64_t * ids, uint32_t count) override {
        ++containerReads;
        std::ifstream file(path); std::string kind; uint64_t id = 0; file >> kind >> id;
        if (!containerKeysAvailable || !id) return 0;
        if (ids && count) ids[0] = id;
        return 1;
    }
};
class Modules : public ISystemModules {
public:
    LoaderProxy loader;
    ISystemloader * selected = &loader;
    void StartEmulation() override {} void StopEmulation(bool) override {} bool IsValid() const override { return true; }
    ISystemloader & Systemloader() override { return *selected; }
    IOperatingSystem & OperatingSystem() override { throw std::runtime_error("Unexpected OS"); }
    IVideo & Video() override { throw std::runtime_error("Unexpected video"); }
    ICpu & Cpu() override { throw std::runtime_error("Unexpected CPU"); }
};
class Notifications : public IModuleNotification {
    void DisplayError(const char * message, const char *) override { throw std::runtime_error(message); }
    void BreakPoint(const char *, uint32_t) override { throw std::runtime_error("Breakpoint"); }
    NotificationResponse Query(const char *, const char *) override { return NotificationResponse::No; }
};
std::vector<GameEntry> Scan(GameLibrary & library, Modules & modules, const char * label) {
    auto & counter = static_cast<LoaderProxy &>(*modules.selected);
    counter.reads = counter.containerReads = 0;
    // Each scan must report whether the loader's recursive index was rebuilt.
    counter.indexRefreshes = 0;
    counter.romReadsByPath.clear();
    const auto started = std::chrono::steady_clock::now();
    library.StartScan(modules);
    while (library.Scanning()) {
        Check(std::chrono::steady_clock::now() - started < std::chrono::seconds(60), "scan timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<GameEntry> games; Check(library.TakeScanResult(games), "scan result delivered");
    std::cout << label << ": " << games.size() << " games, " << counter.reads << " ROM reads, " << counter.containerReads
              << " container reads, " << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count()/1000.0 << " ms" << std::endl;
    return games;
}
void TestScan(const std::filesystem::path & root) {
    Common::FS::SetAppDirectory(root.string());
    // Desktop path initialization can resolve the real user's data directory.
    // The synthetic scan must never replace their library index or icons.
    Common::FS::SetYuzuPath(Common::FS::YuzuPath::YuzuDir, root);
    Common::FS::SetYuzuPath(Common::FS::YuzuPath::CacheDir, root / "cache");
    Common::FS::SetYuzuPath(Common::FS::YuzuPath::NANDDir, root / "nand");
    Common::FS::SetYuzuPath(Common::FS::YuzuPath::SDMCDir, root / "sdmc");
    auto & store = SettingsStore::GetInstance(); Check(store.Initialize((root/"config.json").string().c_str(), root.string().c_str()), "settings");
    const auto directory = root/"games"; std::filesystem::create_directories(directory);
    UiPreferences::SetStringList(UiPreferences::GameDirectories, {directory.string()});
    Write(directory/"one.dnsp", "game 1"); Write(directory/"two.dnsp", "game 2"); Write(directory/"update.dnsp", "update 1");
    FakeLoader fake; Modules modules; modules.selected = &fake;
    GameLibrary library;
    Check(Scan(library, modules, "cold").size() == 2, "cold games");
    // UI and loader read independent versioned caches. The loader must be
    // able to recover Title IDs without depending on the frontend schema.
    const auto dataRoot = Common::FS::GetYuzuPath(Common::FS::YuzuPath::YuzuDir);
    const auto cacheIndex = dataRoot / "game_library" / "index.json";
    const auto contentIndex = GameContentCacheStore::IndexPath(dataRoot);
    std::ifstream input(cacheIndex, std::ios::binary);
    Check(static_cast<bool>(input), "UI index persisted to disk");
    const std::string serialized((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    JsonValue index;
    Check(JsonReader().Parse(serialized.data(), serialized.data() + serialized.size(), index) &&
          index["schema"].asInt64() == GameContentCacheStore::LibrarySchema &&
          index["containers"].isNull(), "UI index schema does not embed container records");
    GameContentCacheStore::Entries persisted;
    Check(GameContentCacheStore::Read(contentIndex, persisted), "shared content index persisted to disk");
    const auto found = persisted.find((directory / "update.dnsp").string());
    Check(found != persisted.end() && found->second.applications.size() == 1 &&
          found->second.applications[0] == 1 && !found->second.signature.empty(),
          "update-to-game association persisted in independent content cache");
    fake.reads = fake.containerReads = 0;
    const auto warm = Scan(library, modules, "warm");
    Check(warm.size() == 2 && fake.reads == 0 && fake.containerReads == 0 && fake.indexRefreshes == 0,
          "warm scan neither opens content nor rebuilds the loader index");
    const auto emptyModRoot = root / "extra_mod_root";
    std::filesystem::create_directories(emptyModRoot / "0000000000000001");
    Check(library.AddDirectory(emptyModRoot.string()), "new root added");
    Check(Scan(library, modules, "mod root added").size() == 2 && fake.indexRefreshes == 0,
          "mod-only roots are handed off without rescanning the loader");
    Check(library.RemoveDirectory(emptyModRoot.string()), "new root removed");
    Check(Scan(library, modules, "mod root removed").size() == 2 && fake.indexRefreshes == 0,
          "removed mod-only roots need no second loader traversal");
    Check(Scan(library, modules, "roots warm").size() == 2 && fake.indexRefreshes == 0,
          "unchanged roots must not rebuild the loader index");
    // Mod-only changes inside an existing root must invalidate the loader's
    // modification-root lookup without reparsing unchanged game containers.
    const auto titleMods = directory / "mods" / "0000000000000001";
    std::filesystem::create_directories(titleMods / "romfs");
    Check(Scan(library, modules, "mod tree appeared").size() == 2 && fake.indexRefreshes == 0 && fake.reads == 0,
          "new mod roots need no second loader traversal");
    Check(Scan(library, modules, "mods warm").size() == 2 && fake.indexRefreshes == 0,
          "unchanged mods do not rebuild the loader index");
    Write(titleMods / "romfs" / "data.bin", "before");
    Check(Scan(library, modules, "mod file added").size() == 2 && fake.indexRefreshes == 0 && fake.reads == 0,
          "mod files stay live without rebuilding directory lookup");
    Write(titleMods / "romfs" / "data.bin", "after modification");
    Check(Scan(library, modules, "mod file changed").size() == 2 && fake.indexRefreshes == 0 && fake.reads == 0,
          "changed mod files need no directory-lookup rebuild");
    std::filesystem::remove_all(titleMods);
    Check(Scan(library, modules, "mod tree removed").size() == 2 && fake.indexRefreshes == 0 && fake.reads == 0,
          "removed mod trees need no second loader traversal");
    Check(Scan(library, modules, "mods warm again").size() == 2 && fake.indexRefreshes == 0,
          "restored warm scan does not rebuild the index");
    // A temporarily disconnected directory must preserve cached games and
    // retry the loader index after the directory returns.
    const auto offline = root / "games_unavailable";
    const auto readIndex = [&cacheIndex] {
        std::ifstream stream(cacheIndex, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    };
    const std::string cacheBeforeDisconnect = readIndex();
    const auto readContent = [&contentIndex] {
        std::ifstream stream(contentIndex, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    };
    const std::string contentBeforeDisconnect = readContent();
    std::filesystem::rename(directory, offline);
    Check(Scan(library, modules, "root disconnected").size() == 2 && fake.reads == 0,
          "missing root retains previous games without reparsing");
    Check(readIndex() == cacheBeforeDisconnect && readContent() == contentBeforeDisconnect,
          "disconnected root must not overwrite UI or loader content metadata");
    std::filesystem::rename(offline, directory);
    Check(Scan(library, modules, "root reconnected").size() == 2 && fake.indexRefreshes == 0 && fake.reads == 0,
          "restored roots reuse cached games without a second traversal");

    Write(directory/"three.dnsp", "game 3"); fake.reads = 0;
    Check(Scan(library, modules, "add").size() == 3 && fake.reads == 1, "add reads only new game");
    std::filesystem::remove(directory/"three.dnsp"); fake.reads = 0;
    Check(Scan(library, modules, "remove").size() == 2 && fake.reads == 0, "remove reads no games");
    Write(directory/"update.dnsp", "update 1 changed"); fake.reads = 0;
    Check(Scan(library, modules, "update").size() == 2 && fake.reads == 2, "update reads only update and affected game");
    GameLibrary reloaded; fake.reads = 0;
    Check(Scan(reloaded, modules, "reloaded").size() == 2 && fake.reads == 0, "persisted cache reused");
    JsonValue loader;
    loader["CheckForUpdatedFirmware"] = false;
    store.SetSettings("nxemu-loader", loader);
    Scan(library, modules, "unrelated preference");
    Check(fake.reads == 0, "firmware-check preference must not invalidate game metadata");
    loader["DisabledAddOns"]["0000000000000001"].Append(JsonValue("Update"));
    store.SetSettings("nxemu-loader", loader);
    Scan(library, modules, "disable update");
    Check(fake.reads == 1, "disabled update invalidates only its game");
}
void TestCrossDirectoryUpdates(const std::filesystem::path & root) {
    const auto games = root / "base_games";
    const auto addons = root / "external_addons";
    std::filesystem::create_directories(games);
    std::filesystem::create_directories(addons / "nested");
    Write(games / "one.dnsp", "game 11");
    Write(games / "two.dnsp", "game 22");
    Write(addons / "one-update.dnsp", "update 11");
    FakeLoader fake; Modules modules; modules.selected = &fake;

    const auto test_order = [&](bool addons_first) {
        UiPreferences::SetStringList(UiPreferences::GameDirectories,
                                     addons_first ? std::vector<std::string>{addons.string(), games.string()}
                                                  : std::vector<std::string>{games.string(), addons.string()});
        GameLibrary library;
        const auto cold = Scan(library, modules, addons_first ? "addon-first" : "game-first");
        Check(cold.size() == 2, "cross-root scan must only show games");
        Check(std::any_of(cold.begin(), cold.end(), [](const auto & game) { return game.programId == 11; }), "first game present");
        Check(std::any_of(cold.begin(), cold.end(), [](const auto & game) { return game.programId == 22; }), "second game present");
        Check(Scan(library, modules, "cross-root warm").size() == 2 && fake.reads == 0 &&
              fake.containerReads == 0 && fake.indexRefreshes == 0,
              "warm cross-root scan must not rebuild the loader index");
        // An update changed in another root must invalidate only its own game.
        Write(addons / "one-update.dnsp", addons_first ? "update 11 modified again" : "update 11 modified");
        Scan(library, modules, "cross-root update changed");
        Check(fake.romReadsByPath[(games / "one.dnsp").string()] == 1 &&
              fake.romReadsByPath[(games / "two.dnsp").string()] == 0,
              "update change must refresh only its associated game");
        Check(Scan(library, modules, "cross-root warm after edit").size() == 2 && fake.reads == 0 && fake.containerReads == 0,
              "second warm scan should not reopen files");
        std::filesystem::remove(addons / "one-update.dnsp");
        Scan(library, modules, "cross-root update removed");
        Check(fake.romReadsByPath[(games / "one.dnsp").string()] == 1 &&
              fake.romReadsByPath[(games / "two.dnsp").string()] == 0,
              "removed update must refresh only its associated game");
        Write(addons / "one-update.dnsp", "update 11 reinstated");
        Scan(library, modules, "cross-root update restored");
        Check(fake.romReadsByPath[(games / "one.dnsp").string()] == 1 &&
              fake.romReadsByPath[(games / "two.dnsp").string()] == 0,
              "restoring update must refresh only its associated game");
        Check(library.RemoveDirectory(addons.string()), "remove configured addon root");
        Check(Scan(library, modules, "cross-root addon directory removed").size() == 2 &&
              fake.romReadsByPath[(games / "one.dnsp").string()] == 1 &&
              fake.romReadsByPath[(games / "two.dnsp").string()] == 0,
              "removing addon root preserves games and refreshes associated version");
    };
    test_order(false);
    test_order(true);
    // Nested add-ons must be indexed, although the game browser stays flat.
    Write(addons / "nested" / "one-extra.dnsp", "update 11 nested");
    UiPreferences::SetStringList(UiPreferences::GameDirectories, {addons.string(), games.string()});
    GameLibrary nestedLibrary;
    Scan(nestedLibrary, modules, "nested addon added");
    Check(fake.romReadsByPath[(games / "one.dnsp").string()] == 1 &&
          fake.romReadsByPath[(games / "two.dnsp").string()] == 0,
          "nested content must refresh only the matching game");
}

void TestTemporaryMissingKeys(const std::filesystem::path & root) {
    const auto gameRoot = root / "keys_games";
    const auto updateRoot = root / "keys_updates";
    std::filesystem::create_directories(gameRoot);
    std::filesystem::create_directories(updateRoot);
    Write(gameRoot / "one.dnsp", "game 33");
    Write(updateRoot / "update.dnsp", "update 33");
    UiPreferences::SetStringList(UiPreferences::GameDirectories,
                                 {gameRoot.string(), updateRoot.string()});
    FakeLoader fake;
    Modules modules;
    modules.selected = &fake;
    GameLibrary library;
    fake.containerKeysAvailable = false;
    Check(Scan(library, modules, "missing keys").size() == 1,
          "base game must still be listed without container metadata");
    fake.containerKeysAvailable = true;
    Scan(library, modules, "keys restored");
    Check(fake.romReadsByPath[(gameRoot / "one.dnsp").string()] == 1,
          "missing-key containers must be re-read and affect matching games");
    Check(Scan(library, modules, "keys warm").size() == 1 &&
          fake.reads == 0 && fake.containerReads == 0,
          "metadata discovered after keys become available must persist");
}

class SnapshotLoader : public FakeLoader {
public:
    size_t publications = 0;
    GameContentSnapshot last;
    bool PublishGameContentSnapshot(const GameContentSnapshot & snapshot) override {
        ++publications;
        last = snapshot;
        return true;
    }
};

void TestCompleteSnapshotHandoff(const std::filesystem::path & root) {
    const auto games = root / "snapshot_games";
    const auto updates = root / "snapshot_updates";
    std::filesystem::create_directories(games);
    std::filesystem::create_directories(updates);
    Write(games / "one.dnsp", "game 41");
    Write(updates / "one-update.dnsp", "update 41");
    UiPreferences::SetStringList(UiPreferences::GameDirectories, {updates.string(), games.string()});
    SnapshotLoader loader;
    Modules modules;
    modules.selected = &loader;
    GameLibrary library;
    Check(Scan(library, modules, "snapshot cold snapshot").size() == 1, "snapshot scan has the game");
    Check(loader.publications == 1 && loader.indexRefreshes == 0,
          "library listing must be handed to loader without a second directory traversal");
    Check(loader.last.directories.size() == 2 && loader.last.files.size() == 2,
          "snapshot includes both roots and both containers");
    bool foundUpdate = false;
    for (const auto & item : loader.last.files)
        if (item.path == (updates / "one-update.dnsp").string())
            foundUpdate = item.applications.size() == 1 && item.applications[0] == 41 && !item.signature.empty();
    Check(foundUpdate, "snapshot reuses actual parsed title association");
    Scan(library, modules, "snapshot warm snapshot");
    Check(loader.indexRefreshes == 0 && loader.containerReads == 0,
          "warm handoff does not reparse containers or recursively refresh loader");
    Write(updates / "one-update.dnsp", "update 41 changed longer");
    Scan(library, modules, "snapshot update changed");
    Check(loader.indexRefreshes == 0 && loader.containerReads == 1,
          "update edit parses only changed container and does not traverse loader roots");
    std::filesystem::create_directories(updates / "mods" / "0000000000000029" / "romfs");
    Scan(library, modules, "snapshot mods appeared");
    Check(loader.indexRefreshes == 0 &&
          std::find(loader.last.modificationRoots.begin(), loader.last.modificationRoots.end(),
                    (updates / "mods" / "0000000000000029").string()) != loader.last.modificationRoots.end(),
          "mod roots are transferred without a second filesystem scan");
    const size_t beforeFailure = loader.publications;
    std::filesystem::rename(updates, root / "snapshot_updates_offline");
    Scan(library, modules, "snapshot incomplete");
    Check(loader.publications == beforeFailure, "incomplete scan cannot publish a partial loader snapshot");
    std::filesystem::rename(root / "snapshot_updates_offline", updates);
    Scan(library, modules, "snapshot reconnected");
    Check(loader.publications > beforeFailure && loader.indexRefreshes == 0,
          "full snapshot is republished after reconnect");
}


void TestScale(const std::filesystem::path & root) {
    for (int count : {100, 1000, 5000}) {
        const auto directory = root / ("scale-" + std::to_string(count));
        std::filesystem::create_directories(directory);
        for (int i=0;i<count;++i) {
            Write(directory/("game-"+std::to_string(i)+".dnsp"), "game "+std::to_string(100+i));
            Write(directory/("update-"+std::to_string(i)+".dnsp"), "update "+std::to_string(100+i));
        }
        UiPreferences::SetStringList(UiPreferences::GameDirectories, {directory.string()});
        SnapshotLoader loader; Modules modules; modules.selected=&loader;
        GameLibrary library;
        Check(Scan(library,modules,("scale cold "+std::to_string(count)).c_str()).size()==size_t(count), "scale game count");
        Check(Scan(library,modules,"scale warm").size()==size_t(count) && loader.reads==0 && loader.containerReads==0,
              "warm scan must not parse unchanged containers regardless of library size");
        Write(directory/"update-0.dnsp", "update 100 changed");
        Scan(library,modules,"scale one update");
        Check(loader.romReadsByPath[(directory/"game-0.dnsp").string()]==1 && loader.reads==2 && loader.containerReads==1,
              "one update changes one game, independent of library size");
    }
}

void TestCancelledSnapshot(const std::filesystem::path & root) {
    class BlockingContainer : public SnapshotLoader {
    public:
        std::latch entered{1}, resume{1};
        bool block=false;
        uint32_t GetContainerApplications(const char * path,uint64_t * ids,uint32_t count) override {
            if (block) { entered.count_down(); resume.wait(); }
            return FakeLoader::GetContainerApplications(path,ids,count);
        }
    } loader;
    const auto directory=root/"cancelled";
    std::filesystem::create_directories(directory);
    Write(directory/"game.dnsp","game 91");
    UiPreferences::SetStringList(UiPreferences::GameDirectories,{directory.string()});
    Modules modules; modules.selected=&loader;
    GameLibrary library;
    Scan(library,modules,"before cancellation");
    const auto before=loader.publications;
    Write(directory/"addon.dnsp","update 91");
    loader.block=true;
    library.StartScan(modules);
    loader.entered.wait();
    std::thread stop([&] { library.StopScan(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    loader.resume.count_down();
    stop.join();
    Check(loader.publications==before,"cancelled indexing must not publish a partial snapshot");
    loader.block=false;
    Check(Scan(library,modules,"after cancellation").size()==1,"scan recovers after cancellation");
}


void TestRejectedSnapshot(const std::filesystem::path & root) {
    class RejectingLoader : public FakeLoader {
        bool PublishGameContentSnapshot(const GameContentSnapshot &) override { return false; }
    } loader;
    const auto directory=root/"superseded";
    std::filesystem::create_directories(directory);
    Write(directory/"game.dnsp","game 92");
    UiPreferences::SetStringList(UiPreferences::GameDirectories,{directory.string()});
    Modules modules; modules.selected=&loader;
    GameLibrary library;
    library.StartScan(modules);
    const auto started=std::chrono::steady_clock::now();
    while (library.Scanning()) {
        Check(std::chrono::steady_clock::now()-started<std::chrono::seconds(5),"superseded scan must finish");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<GameEntry> ignored;
    Check(!library.TakeScanResult(ignored),"superseded scan must not publish stale UI results");
}

void TestBackgroundModuleLifetime() {
    class BlockingLoader : public LoaderProxy {
    public:
        std::latch entered{1}, resume{1};
        IRomInfo * RomInfo(const char *, uint64_t, uint64_t) override {
            entered.count_down();
            resume.wait();
            return nullptr;
        }
    } loader;
    Modules modules; modules.selected = &loader;
    GameLibrary library;
    library.RequestDetails(modules, "slow-container.dnsp");
    loader.entered.wait();
    const bool busy = library.UsingModules(); // must return while the read is blocked
    loader.resume.count_down();
    library.StopScan();
    Check(busy, "module teardown must be deferred while details are being read");
    Check(!library.UsingModules(), "modules can be released after the worker finishes");
}

// Minimal plaintext NCA headers exercise the actual loader parser and content
// provider without requiring keys, copyrighted ROMs or a running emulation.
void WriteNca(const std::filesystem::path & path, uint64_t id, bool dlc=false) {
    std::array<char,0x400> bytes{};
    std::memcpy(bytes.data()+0x200,"DNCA",4);
    bytes[0x205]=dlc ? 4 : 0;
    const uint64_t size=bytes.size();
    const uint32_t sdk=0x000B0000;
    std::memcpy(bytes.data()+0x208,&size,sizeof(size));
    std::memcpy(bytes.data()+0x210,&id,sizeof(id));
    std::memcpy(bytes.data()+0x21c,&sdk,sizeof(sdk));
    std::ofstream out(path,std::ios::binary);
    out.write(bytes.data(),bytes.size());
    Check(bool(out),"NCA fixture");
}

void TestRealLoader(const std::filesystem::path & root,const char * modulePath) {
    const auto data=root/"real-loader";
    const auto games=data/"games", addons=data/"addons";
    std::filesystem::create_directories(games);
    std::filesystem::create_directories(addons);
    auto & store=SettingsStore::GetInstance();
    Check(store.Initialize((data/"config.json").string().c_str(),data.string().c_str()),"real test settings");
    store.SetString(NXCoreSetting::AppDirectory,data.string().c_str());
    Common::FS::SetAppDirectory(data.string());
    for (const auto & [kind,path] : std::initializer_list<std::pair<Common::FS::YuzuPath,std::filesystem::path>>{
            {Common::FS::YuzuPath::YuzuDir,data},{Common::FS::YuzuPath::CacheDir,data/"cache"},
            {Common::FS::YuzuPath::NANDDir,data/"nand"},{Common::FS::YuzuPath::SDMCDir,data/"sdmc"}})
        Common::FS::SetYuzuPath(kind,path);
    constexpr uint64_t base=0x0100000000010000ull;
    const auto game=games/"game.nca", update=addons/"update.nca", dlc=addons/"dlc.nca";
    WriteNca(game,base); WriteNca(update,base|0x800); WriteNca(dlc,base|0x1001,true);
    store.SetString(NXCoreSetting::LogFilter,"*:Critical");
    Common::FS::SetYuzuPath(Common::FS::YuzuPath::LogDir,data/"log");
    Common::Log::Initialize(nullptr,"*:Critical");
    Notifications notifications; ModuleSettings settings; LoaderModule module;
    Check(module.Load(modulePath,&notifications,&settings),"load real loader module");
    Modules modules;
    auto * loader=module.CreateSystemLoader(modules);
    modules.loader.real=loader;
    Check(loader && loader->Initialize(),"real loader initialize");
    // Sparse bases must never read a disabled update or silently switch to an
    // older executable when the selected update has no usable ExeFS.
    {
        const auto bytesOf=[](const std::filesystem::path & path) {
            std::ifstream input(path,std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)),{});
        };
        const auto baseBytes=bytesOf(game), updateBytes=bytesOf(update);
        size_t newerReads=0, olderReads=0;
        auto & provider=loader->ManualContentProvider();
        provider.ClearAllEntries();
        for (const auto & [version,count] : {std::pair{2u,&newerReads},std::pair{1u,&olderReads}})
            provider.AddEntry(LoaderTitleType::Update,LoaderContentRecordType::Program,base|0x800,version,
                new CountingFile(loader->CreateMemoryFile(updateBytes.data(),updateBytes.size(),"update.nca"),*count));
        const auto attempt=[&](const std::vector<const char *> & disabled,bool newer,bool older) {
            loader->SetDisabledAddons(base,disabled.data(),static_cast<uint32_t>(disabled.size()));
            auto * file=loader->CreateMemoryFile(baseBytes.data(),baseBytes.size(),"game.nca");
            auto * rom=loader->FileRomInfo(file,0,0); file->Release();
            Check(rom!=nullptr,"sparse base ROM");
            newerReads=olderReads=0;
            GuestProcessLoadParameters parameters{};
            const auto status=rom->Load(modules,&parameters); rom->Release();
            Check(status==LoaderResultStatus::ErrorNoExeFS,"sparse base without enabled usable ExeFS fails");
            std::cout << "sparse ExeFS: disabled=" << disabled.size() << ", newer reads=" << newerReads << ", older reads=" << olderReads << std::endl;
            Check((newerReads>0)==newer && (olderReads>0)==older,"only selected enabled update may be read for ExeFS");
        };
        attempt({},true,false);
        attempt({"Update@2"},false,true);
        attempt({"Update@1","Update@2"},false,false);
        attempt({"Update"},false,false);
        loader->SetDisabledAddons(base,nullptr,0);
        provider.ClearAllEntries();
    }
    const auto checkContent=[&](const char * label,bool expectUpdate=true,bool expectDlc=true) {
        loader->ManualContentProvider().ClearAllEntries();
        std::ifstream input(game,std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        IVirtualFile * file=loader->CreateMemoryFile(bytes.data(),bytes.size(),"game.nca");
        Check(file!=nullptr,"real fixture file");
        IRomInfo * rom=loader->FileRomInfo(file,0,0);
        file->Release();
        Check(rom!=nullptr,"real ROM metadata");
        rom->PrepareManualContent();
        rom->Release();
        const auto count=[&](uint64_t title) { return loader->GetContentProviderEntriesCount(false,{},false,{},true,title); };
        Check((count(base|0x800)>0)==expectUpdate,"real loader update association");
        Check((count(base|0x1001)>0)==expectDlc,"real loader DLC association");
        std::cout<<label<<": update="<<expectUpdate<<", DLC="<<expectDlc<<std::endl;
    };
    for (bool reversed : {false,true}) {
        UiPreferences::SetStringList(UiPreferences::GameDirectories,reversed ?
            std::vector<std::string>{addons.string(),games.string()} : std::vector<std::string>{games.string(),addons.string()});
        loader->RefreshGameContentIndex();
        checkContent(reversed ? "real addon-first" : "real game-first");
    }
    // A removed update must disappear while the DLC remains associated.
    loader->ManualContentProvider().ClearAllEntries();
    std::filesystem::remove(update); loader->RefreshGameContentIndex(); checkContent("real update removed",false);
    WriteNca(update,base|0x800); loader->RefreshGameContentIndex(); checkContent("real update restored");
    // A complete UI handoff must be immediately usable by the real loader.
    GameLibrary library;
    Scan(library,modules,"real UI handoff");
    checkContent("real handoff associations");
    GameContentSnapshot stale;
    stale.directories={(data/"obsolete").string()};
    Check(!loader->PublishGameContentSnapshot(stale),"real loader rejects obsolete configuration");
    checkContent("real rejected snapshot preserves content");
    loader->ManualContentProvider().ClearAllEntries();
    std::filesystem::rename(update,games/"update.nca");
    std::filesystem::rename(dlc,games/"dlc.nca");
    UiPreferences::SetStringList(UiPreferences::GameDirectories,{games.string()});
    loader->RefreshGameContentIndex();
    checkContent("real mixed directory");
    const auto contentIndex=GameContentCacheStore::IndexPath(data);
    const auto written=std::filesystem::last_write_time(contentIndex);
    module.DestroySystemLoader(loader); modules.loader.real=nullptr;
    Check(module.Load(modulePath,&notifications,&settings),"reload real module");
    loader=module.CreateSystemLoader(modules); modules.loader.real=loader;
    Check(loader && loader->Initialize(),"reinitialize real loader");
    loader->RefreshGameContentIndex();
    checkContent("real restart from persisted index");
    Check(std::filesystem::last_write_time(contentIndex)==written,"warm loader restart does not rewrite content cache");
    module.DestroySystemLoader(loader); modules.loader.real=nullptr;
}

void MeasureReal(const std::filesystem::path & root, const char * modulePath, const std::filesystem::path & data,
                 const std::vector<std::string> & directories = {}) {
    Common::FS::SetAppDirectory(data.string());
    auto & store = SettingsStore::GetInstance();
    Check(store.Initialize((data/"config/NxEmu.config").string().c_str(), data.parent_path().string().c_str()), "real config");
    JsonValue ui = store.GetSettings("UI"), loader = store.GetSettings("nxemu-loader");
    // All writes stay in the temporary copy; game files and installed content are read only.
    Check(store.Initialize((root/"config.json").string().c_str(), root.string().c_str()), "temporary config");
    store.SetSettings("UI", ui); store.SetSettings("nxemu-loader", loader);
    store.SetString(NXCoreSetting::AppDirectory, data.string().c_str());
    store.SetString(NXCoreSetting::LogFilter, "*:Critical");
    Common::FS::SetYuzuPath(Common::FS::YuzuPath::LogDir, root/"log");
    Common::Log::Initialize(nullptr, "*:Critical");
    Notifications notifications; ModuleSettings settings; LoaderModule module;
    Check(module.Load(modulePath, &notifications, &settings), "load real module");
    Modules modules; modules.loader.real = module.CreateSystemLoader(modules);
    Check(modules.loader.real && modules.loader.real->Initialize(), "initialize real loader");
    std::filesystem::copy(data/"game_library", root/"game_library", std::filesystem::copy_options::recursive);
    if (std::filesystem::exists(data/"game_content"))
        std::filesystem::copy(data/"game_content", root/"game_content", std::filesystem::copy_options::recursive);
    Common::FS::SetYuzuPath(Common::FS::YuzuPath::YuzuDir, root);
    if (directories.empty()) {
        GameLibrary library; Scan(library, modules, "real first"); Scan(library, modules, "real warm");
    } else {
        // Start with the base alone in a fresh loader, before any addon was read.
        UiPreferences::SetStringList(UiPreferences::GameDirectories, {directories.front()});
        modules.loader.real->RefreshGameContentIndex();
        for (const auto & entry : std::filesystem::directory_iterator(directories.front())) {
            IRomInfo * info = modules.loader.real->RomInfo(entry.path().string().c_str(), 0, 0);
            if (!info) continue;
            IFileSysNACP * control = info->GetBaseControlMetadata();
            std::cout << "fresh base control version: " << (control ? control->GetVersionString() : "unavailable")
                      << ", patches=" << info->GetGamePatches(nullptr, 0) << std::endl;
            if (control) control->Release();
            info->Release();
        }
        UiPreferences::SetStringList(UiPreferences::GameDirectories, directories);
        GameLibrary library;
        const auto games = Scan(library, modules, "real separate roots");
        Check(games.size() == 1, "one real base game");
        const auto before = GameDetailsReader::Read(*modules.loader.real, games.front().path);
        std::cout << "before: " << before.version << ", patches=" << before.addOns.size() << std::endl;
        const auto checkExecutable = [&](bool enabled) {
            auto & loader = *modules.loader.real;
            const char * disabled[] = {"Update"};
            loader.SetDisabledAddons(games.front().programId, disabled, enabled ? 0 : 1);
            loader.ManualContentProvider().ClearAllEntries();
            auto * rom = loader.RomInfo(games.front().path.c_str(), 0, 0);
            Check(rom != nullptr, "real executable fixture");
            rom->PrepareManualContent();
            bool reachedCpu = false;
            LoaderResultStatus result = LoaderResultStatus::Success;
            try {
                GuestProcessLoadParameters parameters{};
                result = rom->Load(modules, &parameters);
            } catch (const std::runtime_error & error) {
                if (std::string(error.what()) != "Unexpected CPU") { rom->Release(); throw; }
                reachedCpu = true; // Stop before executing any game code.
            }
            rom->Release();
            std::cout << "real ExeFS: enabled=" << enabled << ", reached CPU=" << reachedCpu << ", status=" << static_cast<int>(result) << std::endl;
            Check(enabled ? reachedCpu : result == LoaderResultStatus::ErrorNoExeFS,
                  "real enabled BKTR update must supply ExeFS; disabled update must not");
        };
        checkExecutable(false);
        checkExecutable(true);
        Check(library.RemoveDirectory(directories.back()), "remove real addon root");
        const auto remaining = Scan(library, modules, "real addon root removed");
        const auto after = GameDetailsReader::Read(*modules.loader.real, games.front().path);
        std::cout << "after: " << after.version << ", patches=" << after.addOns.size() << std::endl;
        Check(remaining.size() == 1 && after.addOns.empty(), "removed real addons disappear");
        Check(remaining.front().version == after.version, "cached and fresh versions match after removal");
    }
    module.DestroySystemLoader(modules.loader.real);
}
} // namespace
int main(int argc, char ** argv) {
    Common::Log::DisableLoggingInTests();
    TemporaryDirectory temporary;
    try {
        if (argc == 2) TestRealLoader(temporary.path,argv[1]); else if (argc == 3) MeasureReal(temporary.path, argv[1], argv[2]); else if (argc == 5) MeasureReal(temporary.path, argv[1], argv[2], {argv[3], argv[4]}); else { TestScan(temporary.path); TestCrossDirectoryUpdates(temporary.path); TestTemporaryMissingKeys(temporary.path); TestCompleteSnapshotHandoff(temporary.path); TestCancelledSnapshot(temporary.path); TestRejectedSnapshot(temporary.path); TestScale(temporary.path); TestBackgroundModuleLifetime(); }
    } catch (const std::exception & error) { std::cerr << error.what() << std::endl; return 1; }
    return 0;
}
