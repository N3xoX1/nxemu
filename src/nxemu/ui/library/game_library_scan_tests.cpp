#include "game_library.h"
#include "ui_preferences.h"
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/modules/loader_module.h>
#include <nxemu-core/modules/module_settings.h>
#include <nxemu-module-spec/system_loader.h>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/logging/backend.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool ok, const char * message) { if (!ok) throw std::runtime_error(message); }
void Write(const std::filesystem::path & path, const std::string & text) { std::ofstream file(path); file << text; Check(bool(file), "write fixture"); }
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("nxemu-library-scan-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};
class LoaderProxy : public ISystemloader {
public:
    ISystemloader * real = nullptr;
    size_t reads = 0, containerReads = 0;
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
    IRomInfo * RomInfo(const char * fileName, uint64_t, uint64_t) override {
        ++reads;
        std::ifstream file(fileName); std::string kind; uint64_t id = 0; file >> kind >> id;
        return kind == "game" ? new FakeRom(id) : nullptr;
    }
    uint32_t GetContainerApplications(const char * path, uint64_t * ids, uint32_t count) override {
        ++containerReads;
        std::ifstream file(path); std::string kind; uint64_t id = 0; file >> kind >> id;
        if (!id) return 0;
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
    fake.reads = fake.containerReads = 0;
    const auto warm = Scan(library, modules, "warm");
    Check(warm.size() == 2 && fake.reads == 0 && fake.containerReads == 0, "warm opens no files");
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
void MeasureReal(const std::filesystem::path & root, const char * modulePath, const std::filesystem::path & data) {
    Common::FS::SetAppDirectory(data.string());
    auto & store = SettingsStore::GetInstance();
    Check(store.Initialize((data/"config/NxEmu.config").string().c_str(), data.string().c_str()), "real config");
    // All writes stay in the temporary copy; game files and installed content are read only.
    Check(store.Initialize((root/"config.json").string().c_str(), root.string().c_str()), "temporary config");
    JsonValue original; std::ifstream input(data/"config/NxEmu.config"); std::string contents((std::istreambuf_iterator<char>(input)), {});
    Check(JsonReader().Parse(contents.data(), contents.data()+contents.size(), original), "read config");
    JsonValue ui = original["UI"], loader = original["nxemu-loader"];
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
    Common::FS::SetYuzuPath(Common::FS::YuzuPath::YuzuDir, root);
    { GameLibrary library; Scan(library, modules, "real first"); Scan(library, modules, "real warm"); }
    module.DestroySystemLoader(modules.loader.real);
}
} // namespace
int main(int argc, char ** argv) {
    Common::Log::DisableLoggingInTests();
    TemporaryDirectory temporary;
    try {
        if (argc == 3) MeasureReal(temporary.path, argv[1], argv[2]); else { TestScan(temporary.path); TestBackgroundModuleLifetime(); }
    } catch (const std::exception & error) { std::cerr << error.what() << std::endl; return 1; }
    return 0;
}
