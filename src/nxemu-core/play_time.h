#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <unordered_map>

// Time played per application, kept in a JSON file shared by the desktop and Android frontends.
// Each frontend calls SetPlaying with the running game (0 when none or paused) and formats the numbers itself.
class PlayTimeRecords
{
public:
    struct Record
    {
        uint64_t seconds = 0;
        int64_t lastPlayed = 0;
    };

    explicit PlayTimeRecords(std::filesystem::path file);
    ~PlayTimeRecords();

    // Call again while the same game runs: it adds the time since the last call and saves at most once a minute.
    void SetPlaying(uint64_t programId);
    Record Of(uint64_t programId) const;
    void Clear(uint64_t programId);

private:
    PlayTimeRecords(const PlayTimeRecords &) = delete;
    PlayTimeRecords & operator=(const PlayTimeRecords &) = delete;

    void Load();
    void Save();

    const std::filesystem::path m_file;
    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, Record> m_records;
    uint64_t m_playing = 0;
    std::chrono::steady_clock::time_point m_since;
    std::chrono::steady_clock::time_point m_lastSaved;
    bool m_dirty = false;
};
