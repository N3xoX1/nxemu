#include "game_settings.h"
#include "settings.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <yuzu_common/fs/fs_util.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
constexpr uintmax_t kMaxFileBytes = 8 * 1024 * 1024;

JsonValue EmptyDatabase()
{
    JsonValue root;
    root["version"] = 1;
    root["games"] = JsonValue(JsonValueType::Object);
    return root;
}

bool Read(const std::filesystem::path & path, JsonValue & root, bool optional, std::string & error)
{
    std::error_code ec;
    if (optional && !std::filesystem::exists(path, ec) && !ec)
    {
        root = EmptyDatabase();
        return true;
    }
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes > kMaxFileBytes)
    {
        error = "Cannot read override file: " + Common::FS::PathToUTF8String(path);
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(file)), {});
    if (!file || !JsonReader().Parse(contents.data(), contents.data() + contents.size(), root))
    {
        error = "Invalid override JSON: " + Common::FS::PathToUTF8String(path);
        return false;
    }
    return GameSettings::Validate(root, error);
}

const JsonValue & Values(const JsonValue & root, uint64_t id)
{
    return root["games"][GameSettings::TitleId(id)]["settings"];
}

bool DatabaseDisabled(const JsonValue & users, uint64_t id, const char * key)
{
    for (const auto & disabled : users["games"][GameSettings::TitleId(id)]["disabled_nxemu"])
    {
        if (disabled.asString() == key) return true;
    }
    return false;
}

void PruneGame(JsonValue & root, const std::string & id)
{
    const auto & game = root["games"][id];
    if (game["settings"].empty() && game["disabled_nxemu"].empty()) root["games"].removeMember(id.c_str());
}
} // namespace

bool GameSettingDefinition::Accepts(const JsonValue & value) const
{
    return boolean ? value.isBool() : (value.Type() == JsonValueType::Int || value.Type() == JsonValueType::UnsignedInt) && value.isInt() && value.asInt64() >= minimum && value.asInt64() <= maximum;
}

std::string GameSettingDefinition::Display(const JsonValue & value) const
{
    if (boolean)
    {
        return value.asBool() ? "On" : "Off";
    }
    const int64_t number = value.asInt64();
    if (number >= 0 && static_cast<size_t>(number) < choices.size())
    {
        return choices[static_cast<size_t>(number)];
    }
    if (std::string(key) == NXOsSetting::Region && number == -1)
    {
        return "Follow language";
    }
    return std::to_string(number) + (std::string(key) == NXOsSetting::AudioVolume || std::string(key) == NXOsSetting::SpeedLimit ||
                                    std::string(key) == NXVideoSetting::FSPSharpness ? "%" : "");
}

const std::vector<GameSettingDefinition> & GameSettings::Definitions()
{
    static const std::vector<GameSettingDefinition> definitions = {
        {NXOsSetting::DockedMode, "mode-docked", "Console mode", false, 0, 1, {"Handheld", "Docked"}},
        {NXVideoSetting::ResolutionSetup, "resolution", "Resolution scale", false, 0, 10, {"0.5x", "0.75x", "1x", "1.5x", "2x", "3x", "4x", "5x", "6x", "7x", "8x"}},
        {NXVideoSetting::AspectRatio, "aspect", "Aspect ratio", false, 0, 4, {"16:9", "4:3", "21:9", "16:10", "Stretch"}},
        {NXVideoSetting::VSyncMode, "vsync", "VSync", false, 0, 3, {"Off", "Mailbox", "On", "Relaxed"}},
        {NXOsSetting::LanguageIndex, "language", "Console language", false, 0, 17, {"Japanese", "English (US)", "French", "German", "Italian", "Spanish", "Chinese", "Korean", "Dutch", "Portuguese", "Russian", "Taiwanese", "English (UK)", "French (Canada)", "Spanish (Latin America)", "Chinese (Simplified)", "Chinese (Traditional)", "Portuguese (Brazil)"}},
        {NXOsSetting::Region, "region", "Console region", false, -1, 6, {"Japan", "Americas", "Europe", "Australia", "China", "Korea", "Taiwan"}},
        {NXOsSetting::UseSpeedLimit, "limit", "Limit speed", true, 0, 0, {}},
        {NXOsSetting::SpeedLimit, "speed-range", "Speed limit", false, 25, 300, {}},
        {NXOsSetting::AudioMode, "audio-mode", "Audio mode", false, 0, 2, {"Mono", "Stereo", "Surround"}},
        {NXOsSetting::AudioVolume, "volume", "Volume", false, 0, 200, {}},
        {NXOsSetting::AudioMuted, "volume", "Mute", true, 0, 0, {}},
        {NXVideoSetting::ScalingFilter, "filter", "Window filter", false, 0, 5, {"Nearest", "Bilinear", "Bicubic", "Gaussian", "ScaleForce", "FSR"}},
        {NXVideoSetting::FSPSharpness, "sharpness-range", "Sharpness", false, 0, 200, {}},
        {NXVideoSetting::AntiAliasing, "antialiasing", "Anti-aliasing", false, 0, 2, {"None", "FXAA", "SMAA"}},
        {NXVideoSetting::AnisotropicFiltering, "anisotropy", "Anisotropic filtering", false, 0, 5, {"Automatic", "As requested", "2x", "4x", "8x", "16x"}},
        {NXVideoSetting::AccuracyLevel, "accuracy", "GPU accuracy", false, 0, 2, {"Normal", "High", "Extreme"}},
        {NXVideoSetting::DMAAccuracy, "dma", "Memory reads", false, 0, 2, {"As accuracy", "Unsafe", "Safe"}},
        {NXVideoSetting::AstcDecodeMode, "astc", "ASTC decoding", false, 0, 2, {"CPU", "GPU", "CPU asynchronous"}},
        {NXVideoSetting::ASTCRecompressionMethod, "astc-recompression", "ASTC recompression", false, 0, 2, {"None", "BC1", "BC3"}},
        {NXVideoSetting::VRAMUsageMode, "vram", "Graphics memory", false, 0, 1, {"Conservative", "Aggressive"}},
        {NXVideoSetting::NvdecEmulation, "nvdec", "Video decoding", false, 0, 2, {"Off", "CPU", "GPU"}},
        {NXCpuSetting::CpuBackend, "cpu-backend", "Processor engine", false, 0, 1, {"Dynarmic", "NCE"}},
        {NXCpuSetting::CpuAccuracy, "cpu-accuracy", "Processor accuracy", false, 0, 3, {"Automatic", "Accurate", "Unsafe", "Paranoid"}},
        {NXOsSetting::MemoryLayout, "memory", "Console memory", false, 0, 2, {"4 GB", "6 GB", "8 GB"}},
#define GAME_BOOL(key, control, label) {key, control, label, true, 0, 0, {}}
        GAME_BOOL(NXVideoSetting::GpuCommandSynchronization, "gpu-sync", "Command synchronization"),
        GAME_BOOL(NXVideoSetting::EnableReactiveFlushing, "reactive-flushing", "Reactive flushing"),
        GAME_BOOL(NXVideoSetting::BarrierFeedbackLoops, "feedback-loops", "Barrier feedback loops"),
        GAME_BOOL(NXVideoSetting::FastGPUTime, "fast-gpu-time", "Fast GPU time"),
        GAME_BOOL(NXVideoSetting::SyncToFramerateOfVideoPlayback, "video-framerate", "Videos at their own speed"),
        GAME_BOOL(NXVideoSetting::UseDiskPipelineCache, "disk-cache", "Disk shader cache"),
        GAME_BOOL(NXVideoSetting::UseVulkanPipelineCache, "driver-cache", "Driver pipeline cache"),
        GAME_BOOL(NXVideoSetting::UseAsynchronousShaderBuilding, "async-shaders", "Background shaders"),
        GAME_BOOL(NXVideoSetting::UseAsynchronousGPUEmulation, "async-gpu", "Graphics thread"),
        GAME_BOOL(NXVideoSetting::EnableAsynchronousPresentation, "async-present", "Presentation thread"),
        GAME_BOOL(NXVideoSetting::ForceMaximumClocks, "max-clocks", "Maximum GPU clocks"),
        GAME_BOOL(NXOsSetting::UseMultiCore, "multicore", "Multicore"),
        GAME_BOOL(NXOsSetting::CpuHleSynchronization, "cpu-hle", "CPU / system synchronization"),
        GAME_BOOL(NXOsSetting::VibrationEnabled, "vibration", "Vibration"),
        GAME_BOOL(NXOsSetting::EnableAccurateVibrations, "accurate-vibration", "Accurate vibration"),
#undef GAME_BOOL
    };
    return definitions;
}

GameSettings & GameSettings::GetInstance()
{
    static GameSettings instance;
    return instance;
}

const GameSettingDefinition * GameSettings::Definition(const char * key)
{
    for (const auto & definition : Definitions())
    {
        if (key != nullptr && std::string(key) == definition.key)
        {
            return &definition;
        }
    }
    return nullptr;
}

std::string GameSettings::TitleId(uint64_t programId)
{
    char id[17];
    std::snprintf(id, sizeof(id), "%016llX", static_cast<unsigned long long>(programId));
    return id;
}

bool GameSettings::Validate(const JsonValue & root, std::string & error)
{
    if (!root.isObject() || (root["version"].Type() != JsonValueType::Int && root["version"].Type() != JsonValueType::UnsignedInt) || root["version"].asInt64() != 1 || !root["games"].isObject())
    {
        error = "Override database requires version 1 and a games object.";
        return false;
    }
    for (const auto & id : root["games"].GetMemberNames())
    {
        if (id.size() != 16 || id == "0000000000000000" ||
            !std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }))
        {
            error = "Invalid title ID (expected 16 uppercase hex digits): " + id;
            return false;
        }
        const auto & game = root["games"][id];
        if (!game.isObject() || !game["settings"].isObject())
        {
            error = "Missing settings object for " + id;
            return false;
        }
        for (const auto & key : game["settings"].GetMemberNames())
        {
            const auto * definition = Definition(key.c_str());
            if (definition == nullptr || !definition->Accepts(game["settings"][key]))
            {
                error = "Unknown setting, invalid type or out-of-range value: " + id + "/" + key;
                return false;
            }
        }
        if (game.isMember("disabled_nxemu"))
        {
            const auto & disabled = game["disabled_nxemu"];
            std::vector<std::string> seen;
            if (!disabled.isArray())
            {
                error = "disabled_nxemu must be an array: " + id;
                return false;
            }
            for (const auto & key : disabled)
            {
                const std::string name = key.isString() ? key.asString() : "";
                if (!key.isString() || Definition(name.c_str()) == nullptr || std::find(seen.begin(), seen.end(), name) != seen.end())
                {
                    error = "Invalid or duplicate disabled NXEmu setting: " + id;
                    return false;
                }
                seen.push_back(name);
            }
        }
    }
    return true;
}

bool GameSettings::Initialize(const std::filesystem::path & database, const std::filesystem::path & userFile, std::string & error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_database = EmptyDatabase();
    m_users = EmptyDatabase();
    m_userFile = userFile;
    JsonValue databaseRoot, userRoot;
    std::string databaseError, userError;
    const bool validDatabase = Read(database, databaseRoot, false, databaseError);
    m_userWritable = Read(userFile, userRoot, true, userError);
    if (validDatabase) m_database = std::move(databaseRoot);
    if (m_userWritable) m_users = std::move(userRoot);
    error = databaseError + (databaseError.empty() || userError.empty() ? "" : "\n") + userError;
    return validDatabase && m_userWritable;
}

JsonValue GameSettings::Snapshot(uint64_t programId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    JsonValue values = Values(m_database, programId);
    for (const auto & key : m_users["games"][TitleId(programId)]["disabled_nxemu"])
    {
        values.removeMember(key.asString().c_str());
    }
    const auto & users = Values(m_users, programId);
    for (const auto & key : users.GetMemberNames()) values[key] = users[key];
    return values;
}

JsonValue GameSettings::Resolve(uint64_t programId, const char * key) const
{
    const JsonValue values = Snapshot(programId);
    if (values.isMember(key)) return values[key];
    SettingsStore::GlobalReadScope global;
    const auto & store = SettingsStore::GetInstance();
    const auto * definition = Definition(key);
    return definition != nullptr && definition->boolean ? JsonValue(store.GetBool(key)) : JsonValue(store.GetInt(key));
}

GameSettings::Source GameSettings::Origin(uint64_t programId, const char * key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return Values(m_users, programId).isMember(key) ? Source::User :
           Values(m_database, programId).isMember(key) && !DatabaseDisabled(m_users, programId, key) ? Source::NXEmu : Source::Global;
}

std::vector<GameSettings::Entry> GameSettings::Entries(uint64_t programId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Entry> entries;
    for (bool user : {false, true})
    {
        const auto & values = Values(user ? m_users : m_database, programId);
        for (const auto & definition : Definitions())
        {
            if (values.isMember(definition.key))
            {
                const bool enabled = user || !DatabaseDisabled(m_users, programId, definition.key);
                entries.push_back({definition.key, definition.label, definition.Display(values[definition.key]),
                                   user ? Source::User : Source::NXEmu, enabled && (user || !Values(m_users, programId).isMember(definition.key)), enabled});
            }
        }
    }
    return entries;
}

bool GameSettings::SaveUser(const JsonValue & root, std::string & error)
{
    if (!m_userWritable)
    {
        error = "The user override file could not be read. Fix it before saving overrides.";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(m_userFile.parent_path(), ec);
    auto temporary = m_userFile;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const std::string json = JsonStyledWriter().write(root);
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    output.close();
    if (!output || ec)
    {
        error = "Cannot write user overrides.";
        std::filesystem::remove(temporary, ec);
        return false;
    }
#ifdef _WIN32
    const bool renamed = MoveFileExW(temporary.c_str(), m_userFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, m_userFile, ec);
    const bool renamed = !ec;
#endif
    if (!renamed)
    {
        error = "Cannot replace the user override file.";
        std::filesystem::remove(temporary, ec);
        return false;
    }
    m_users = root;
    return true;
}

bool GameSettings::SetUser(uint64_t programId, const char * key, const JsonValue & value, std::string & error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto * definition = Definition(key);
    if (programId == 0 || definition == nullptr || !definition->Accepts(value))
    {
        error = "Invalid game override.";
        return false;
    }
    auto root = m_users;
    root["games"][TitleId(programId)]["settings"][key] = value;
    return SaveUser(root, error);
}

bool GameSettings::Inherit(uint64_t programId, const char * key, std::string & error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto root = m_users;
    const auto id = TitleId(programId);
    if (key == nullptr)
    {
        root["games"].removeMember(id.c_str());
    }
    else
    {
        if (!Values(root, programId).isMember(key)) return true;
        root["games"][id]["settings"].removeMember(key);
        PruneGame(root, id);
    }
    return SaveUser(root, error);
}

bool GameSettings::SetDatabaseEnabled(uint64_t programId, const char * key, bool enabled, std::string & error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (programId == 0 || key == nullptr || !Values(m_database, programId).isMember(key))
    {
        error = "Unknown NXEmu override.";
        return false;
    }
    if (DatabaseDisabled(m_users, programId, key) == !enabled) return true;
    auto root = m_users;
    const auto id = TitleId(programId);
    auto & game = root["games"][id];
    if (!game["settings"].isObject()) game["settings"] = JsonValue(JsonValueType::Object);
    JsonValue disabled(JsonValueType::Array);
    const JsonValue & previous = game["disabled_nxemu"];
    for (const auto & value : previous)
    {
        if (value.asString() != key) disabled.Append(value);
    }
    if (!enabled) disabled.Append(JsonValue(key));
    if (disabled.empty()) game.removeMember("disabled_nxemu");
    else game["disabled_nxemu"] = std::move(disabled);
    PruneGame(root, id);
    return SaveUser(root, error);
}
