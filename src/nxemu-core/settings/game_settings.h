#pragma once
#include <common/json.h>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

// Stable identifiers and typed values are shared by the shipped database,
// user overrides and the existing settings controls. Machine preferences
// (devices, paths, profiles and application behaviour) stay global.
struct GameSettingDefinition
{
    const char * key;
    const char * control;
    const char * label;
    bool boolean;
    int minimum;
    int maximum;
    std::vector<const char *> choices;
    bool Accepts(const JsonValue & value) const;
    std::string Display(const JsonValue & value) const;
};

class GameSettings
{
public:
    enum class Source { Global, NXEmu, User };
    struct Entry
    {
        std::string key, name, value;
        Source source;
        bool effective;
        bool enabled;
    };

    static GameSettings & GetInstance();
    static const std::vector<GameSettingDefinition> & Definitions();
    static const GameSettingDefinition * Definition(const char * key);
    static std::string TitleId(uint64_t programId);
    static bool Validate(const JsonValue & root, std::string & error);

    // A malformed user file is never overwritten by the editor.
    bool Initialize(const std::filesystem::path & database, const std::filesystem::path & userFile, std::string & error);
    JsonValue Resolve(uint64_t programId, const char * key) const;
    Source Origin(uint64_t programId, const char * key) const;
    JsonValue Snapshot(uint64_t programId) const;
    std::vector<Entry> Entries(uint64_t programId) const; // NXEmu first, User second
    bool SetUser(uint64_t programId, const char * key, const JsonValue & value, std::string & error);
    bool SetDatabaseEnabled(uint64_t programId, const char * key, bool enabled, std::string & error);
    bool Inherit(uint64_t programId, const char * key, std::string & error); // null key resets the whole game

private:
    bool SaveUser(const JsonValue & root, std::string & error);
    mutable std::mutex m_mutex;
    JsonValue m_database, m_users;
    std::filesystem::path m_userFile;
    bool m_userWritable = false;
};
