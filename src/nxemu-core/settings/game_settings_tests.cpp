#include "game_settings.h"
#include "settings.h"
#include <nxemu-core/modules/module_settings.h>
#include <nxemu-cpu/cpu_settings.h>
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-video/video_settings.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

IModuleSettings * g_settings = nullptr;

namespace
{
void Check(bool condition, const char * message)
{
    if (!condition) throw std::runtime_error(message);
}

void Write(const std::filesystem::path & path, const JsonValue & root)
{
    std::ofstream file(path);
    file << JsonStyledWriter().write(root);
    Check(static_cast<bool>(file), "write fixture");
}

JsonValue Read(const std::filesystem::path & path)
{
    std::ifstream file(path);
    std::string text((std::istreambuf_iterator<char>(file)), {});
    JsonValue root;
    Check(JsonReader().Parse(text.data(), text.data() + text.size(), root), "parse saved file");
    return root;
}
} // namespace

int main(int argc, char ** argv)
{
    const auto temporary = std::filesystem::temp_directory_path() /
        ("nxemu-overrides-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temporary);
    try
    {
        Check(argc == 2, "expected the shipped database path");
        constexpr uint64_t title = 0x0100000000010000;
        const auto database = temporary / "database.json";
        const auto users = temporary / "users.json";
        const auto config = temporary / "NxEmu.config";
        auto root = Read(argv[1]);
        std::string error;
        Check(GameSettings::Validate(root, error), "shipped database is valid");
        Check(root["games"][GameSettings::TitleId(title)]["settings"][NXVideoSetting::AccuracyLevel].asInt64() == 1, "shipped profile uses High");
        root["games"][GameSettings::TitleId(title)]["settings"][NXVideoSetting::GpuCommandSynchronization] = true;
        root["games"][GameSettings::TitleId(title)]["settings"][NXCpuSetting::CpuAccuracy] = 1;
        root["games"][GameSettings::TitleId(title)]["settings"][NXOsSetting::MemoryLayout] = 2;
        Write(database, root);
        GameSettings games;
        Check(games.Initialize(database, users, error), "load database and absent user file");
        auto & store = SettingsStore::GetInstance();
        Check(store.Initialize(config.string().c_str(), temporary.string().c_str()), "initialize globals");
        JsonValue video;
        video["video"]["accuracy_level"] = "Extreme";
        video["video"]["gpu_command_synchronization"] = false;
        store.SetSettings("nxemu-video", video);
        store.SetDefaultInt(NXOsSetting::MemoryLayout, 0);
        store.SetDefaultBool(NXOsSetting::UseMultiCore, true);
        ModuleSettings moduleSettings;
        g_settings = &moduleSettings;
        SetupVideoSetting();
        SetupCpuSetting();
        Check(games.SetUser(title, NXVideoSetting::UseAsynchronousShaderBuilding, JsonValue(true), error), "per-game asynchronous shaders");
        Check(games.SetUser(title, NXCpuSetting::CpuBackend, JsonValue(1), error), "per-game native processor");
        Check(games.Resolve(title, NXVideoSetting::AccuracyLevel).asInt64() == 1, "NXEmu beats global Extreme");
        Check(games.Resolve(title + 1, NXVideoSetting::AccuracyLevel).asInt64() == 2, "unlisted game uses global");
        Check(games.Resolve(title + 1, NXOsSetting::MemoryLayout).asInt64() == 0, "factory default fallback");
        Check(games.SetUser(title, NXVideoSetting::AccuracyLevel, JsonValue(0), error), "user int zero");
        Check(games.SetUser(title, NXVideoSetting::GpuCommandSynchronization, JsonValue(false), error), "user false");
        Check(games.SetUser(title, NXOsSetting::UseMultiCore, JsonValue(true), error), "explicit equal-to-inherited value");
        Check(games.Origin(title, NXOsSetting::UseMultiCore) == GameSettings::Source::User, "equal inherited value stays explicit");
        Check(games.Resolve(title, NXVideoSetting::AccuracyLevel).asInt64() == 0, "User beats NXEmu");
        Check(!games.Resolve(title, NXVideoSetting::GpuCommandSynchronization).asBool(), "false beats true");
        const auto entries = games.Entries(title);
        bool sawUser = false, sawShadowed = false;
        for (const auto & entry : entries)
        {
            if (entry.source == GameSettings::Source::User) sawUser = true;
            else Check(!sawUser, "NXEmu entries come first");
            if (entry.key == NXVideoSetting::AccuracyLevel && entry.source == GameSettings::Source::NXEmu) sawShadowed = !entry.effective;
        }
        Check(sawUser && sawShadowed, "both layers are shown, with shadowed status");
        CleanupVideoSetting();
        CleanupCpuSetting();
        store.SetGameOverrides(games.Snapshot(title));
        SetupVideoSetting();
        SetupCpuSetting();
        Check(videoSettings.gpu_accuracy == GpuAccuracy::Normal, "video module applies zero before initialization");
        Check(videoSettings.use_asynchronous_shaders && cpuSettings.cpu_backend == CpuBackend::Nce, "CPU and shader options apply together at game start");
        // The actual video module must react live, including when a global edit
        // would otherwise be hidden by an explicit status bar choice.
        JsonValue snapshot = games.Snapshot(title);
        snapshot[NXVideoSetting::ScalingFilter] = static_cast<int>(ScalingFilter::Bicubic);
        CleanupVideoSetting();
        store.SetGameOverrides(snapshot);
        SetupVideoSetting();
        Check(videoSettings.scaling_filter == ScalingFilter::Bicubic, "video module reads game filter");
        store.SetControlOverride(NXVideoSetting::ScalingFilter, JsonValue(static_cast<int>(ScalingFilter::Bilinear)));
        Check(videoSettings.scaling_filter == ScalingFilter::Bilinear, "explicit filter applies live over game snapshot");
        store.SetInt(NXVideoSetting::ScalingFilter, static_cast<int>(ScalingFilter::Fsr));
        Check(videoSettings.scaling_filter == ScalingFilter::Bilinear, "global changes cannot replace explicit filter");
        {
            SettingsStore::GlobalReadScope global;
            SaveVideoSettings();
        }
        store.ClearControlOverride(NXVideoSetting::ScalingFilter);
        Check(videoSettings.scaling_filter == ScalingFilter::Bicubic, "reset restores game filter live");
        CleanupVideoSetting();
        SetupVideoSetting();
        Check(videoSettings.scaling_filter == ScalingFilter::Bicubic, "game filter survives module reinitialization");
        Check(!Settings::IsGpuCommandSynchronizationEnabled(), "runtime GPU snapshot applies false");
        Check(cpuSettings.cpu_accuracy == CpuAccuracy::Accurate, "CPU module applies database override");
        Check(store.GetInt(NXOsSetting::MemoryLayout) == 2, "memory override ready before OS construction");
        Check(games.SetUser(title, NXVideoSetting::AccuracyLevel, JsonValue(2), error), "edit future launch");
        Check(store.GetInt(NXVideoSetting::AccuracyLevel) == 0, "running snapshot stays immutable");
        // Save the real module serializers during emulation. Global edits must
        // survive even when their key is masked by a game override.
        store.SetInt(NXVideoSetting::AccuracyLevel, 1);
        store.SetInt(NXCpuSetting::CpuAccuracy, 2);
        {
            SettingsStore::GlobalReadScope global;
            Check(store.GetInt(NXVideoSetting::AccuracyLevel) == 1, "editor reads global");
            bool emulationRead = false;
            std::thread emulation([&] { emulationRead = store.GetInt(NXVideoSetting::AccuracyLevel) == 0; });
            emulation.join();
            Check(emulationRead, "global read scope cannot affect another thread");
            SaveVideoSettings();
            SaveCpuSettings();
        }
        const auto saved = Read(config);
        Check(saved["nxemu-video"]["video"]["accuracy_level"].isNull(), "save global video value, not effective Normal");
        Check(saved["nxemu-cpu"]["cpu"]["cpu_accuracy"].asString() == "Unsafe", "save global CPU value, not effective Accurate");
        Check(videoSettings.gpu_accuracy == GpuAccuracy::Normal && cpuSettings.cpu_accuracy == CpuAccuracy::Accurate, "saving never mutates live modules");
        CleanupVideoSetting();
        CleanupCpuSetting();
        store.SetGameOverrides(JsonValue());
        Check(store.GetInt(NXVideoSetting::AccuracyLevel) == 1, "stop restores global reads");
        GameSettings reloaded;
        Check(reloaded.Initialize(database, users, error), "reload user overrides");
        Check(reloaded.Resolve(title, NXVideoSetting::AccuracyLevel).asInt64() == 2, "saved user value survives restart");
        Check(reloaded.Resolve(title, NXVideoSetting::UseAsynchronousShaderBuilding).asBool() &&
              reloaded.Resolve(title, NXCpuSetting::CpuBackend).asInt64() == 1, "CPU and shader per-game options survive restart");
        Check(reloaded.Inherit(title, NXVideoSetting::AccuracyLevel, error), "reset one key");
        Check(reloaded.Resolve(title, NXVideoSetting::AccuracyLevel).asInt64() == 1, "inherit NXEmu");
        Check(reloaded.Inherit(title, nullptr, error), "reset all user values");
        Check(reloaded.Origin(title, NXOsSetting::UseMultiCore) == GameSettings::Source::Global, "inherit global for absent database key");
        store.SetInt(NXVideoSetting::AccuracyLevel, 2);
        Check(reloaded.SetDatabaseEnabled(title, NXVideoSetting::AccuracyLevel, false, error), "disable one database override");
        Check(reloaded.Origin(title, NXVideoSetting::AccuracyLevel) == GameSettings::Source::Global, "disabled database falls back to global");
        Check(reloaded.Resolve(title, NXVideoSetting::AccuracyLevel).asInt64() == 2, "disabled database resolves global Extreme");
        Check(!reloaded.Snapshot(title).isMember(NXVideoSetting::AccuracyLevel), "disabled database absent from launch snapshot");
        bool sawDisabled = false;
        for (const auto & entry : reloaded.Entries(title))
        {
            if (entry.key == NXVideoSetting::AccuracyLevel) sawDisabled = !entry.enabled && !entry.effective;
        }
        Check(sawDisabled, "disabled database row stays visible");
        Check(reloaded.SetUser(title, NXVideoSetting::AccuracyLevel, JsonValue(0), error), "user can override disabled database");
        Check(reloaded.Resolve(title, NXVideoSetting::AccuracyLevel).asInt64() == 0, "user remains highest priority");
        Check(reloaded.Inherit(title, NXVideoSetting::AccuracyLevel, error), "quick removal of user override");
        Check(reloaded.Resolve(title, NXVideoSetting::AccuracyLevel).asInt64() == 2, "removal preserves database suppression");
        Check(reloaded.Initialize(database, users, error), "reload suppression-only user file");
        Check(reloaded.Resolve(title, NXVideoSetting::AccuracyLevel).asInt64() == 2, "suppression survives restart");
        Check(reloaded.SetDatabaseEnabled(title, NXVideoSetting::AccuracyLevel, true, error), "reenable database override");
        Check(reloaded.Resolve(title, NXVideoSetting::AccuracyLevel).asInt64() == 1, "reenabled database wins again");
        Check(Read(users)["games"].empty(), "remove empty user record on reenable");
        Check(reloaded.SetDatabaseEnabled(title, NXVideoSetting::AccuracyLevel, false, error), "disable before reset all");
        Check(reloaded.Inherit(title, nullptr, error), "reset all also reenables database overrides");
        Check(reloaded.Origin(title, NXVideoSetting::AccuracyLevel) == GameSettings::Source::NXEmu, "reset restores database source");
        Check(JsonStyledWriter().write(Read(database)) == JsonStyledWriter().write(root), "shipped database is never modified");
        Check(!reloaded.SetDatabaseEnabled(title, NXOsSetting::UseMultiCore, false, error), "cannot disable an absent database entry");
        for (JsonValue invalid : {JsonValue("high"), JsonValue(true), JsonValue(-1), JsonValue(3), JsonValue(1.5), JsonValue(1.0)})
        {
            Check(!reloaded.SetUser(title, NXVideoSetting::AccuracyLevel, invalid, error), "reject wrong type/range");
        }
        Check(!reloaded.SetUser(title, "nxcore:ShowLogConsole", JsonValue(true), error), "application settings excluded");
        auto invalid = root;
        invalid["games"]["NOT-A-TITLE"]["settings"][NXVideoSetting::AccuracyLevel] = 1;
        Check(!GameSettings::Validate(invalid, error), "reject invalid title ID");
        invalid = root;
        invalid["version"] = 2;
        Check(!GameSettings::Validate(invalid, error), "reject unsupported schema");
        for (JsonValue disabled : {JsonValue(true), JsonValue("nxvideo:AccuracyLevel"), JsonValue(JsonValueType::Object)})
        {
            invalid = root;
            invalid["games"][GameSettings::TitleId(title)]["disabled_nxemu"] = disabled;
            Check(!GameSettings::Validate(invalid, error), "reject non-array suppression");
        }
        for (JsonValue key : {JsonValue("unknown"), JsonValue(false), JsonValue(JsonValueType::Object)})
        {
            invalid = root;
            invalid["games"][GameSettings::TitleId(title)]["disabled_nxemu"] = JsonValue(JsonValueType::Array);
            invalid["games"][GameSettings::TitleId(title)]["disabled_nxemu"].Append(key);
            Check(!GameSettings::Validate(invalid, error), "reject invalid suppression key");
        }
        invalid = root;
        invalid["games"][GameSettings::TitleId(title)]["disabled_nxemu"].Append(JsonValue(NXVideoSetting::AccuracyLevel));
        invalid["games"][GameSettings::TitleId(title)]["disabled_nxemu"].Append(JsonValue(NXVideoSetting::AccuracyLevel));
        Check(!GameSettings::Validate(invalid, error), "reject duplicate suppression key");
        { std::ofstream file(users); file << "{ broken"; }
        const auto before = std::filesystem::file_size(users);
        Check(!reloaded.Initialize(database, users, error), "report malformed user file");
        Check(!reloaded.SetUser(title, NXVideoSetting::AccuracyLevel, JsonValue(0), error), "never overwrite malformed user data");
        Check(std::filesystem::file_size(users) == before, "invalid file preserved");
        // A failed atomic replacement must not change the in-memory layer.
        std::filesystem::remove(users);
        Check(reloaded.Initialize(database, users, error), "reload with absent file");
        std::filesystem::create_directory(users);
        { std::ofstream file(users / "keep"); file << "keep"; }
        Check(!reloaded.SetUser(title, NXVideoSetting::AccuracyLevel, JsonValue(0), error), "failed replace reports error");
        Check(reloaded.Origin(title, NXVideoSetting::AccuracyLevel) == GameSettings::Source::NXEmu, "failed save leaves state unchanged");
        Check(!reloaded.SetDatabaseEnabled(title, NXVideoSetting::AccuracyLevel, false, error), "failed suppression save reports error");
        Check(reloaded.Origin(title, NXVideoSetting::AccuracyLevel) == GameSettings::Source::NXEmu, "failed suppression save keeps database enabled");
        g_settings = nullptr;
        SettingsStore::CleanUp();
        std::filesystem::remove_all(temporary);
        std::cout << "Game overrides: shipped DB, precedence, persistence, inheritance, validation and real video/CPU serializers passed.\n";
        return 0;
    }
    catch (const std::exception & exception)
    {
        std::cerr << exception.what() << "\nFixtures retained: " << temporary << '\n';
        return 1;
    }
}
