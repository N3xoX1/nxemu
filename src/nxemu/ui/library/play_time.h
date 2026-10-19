#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

class PlayTime
{
public:
    struct Record
    {
        uint64_t seconds = 0;
        int64_t lastPlayed = 0;
    };

    explicit PlayTime(std::filesystem::path file);
    ~PlayTime();

    void SetPlaying(uint64_t programId);
    Record Of(uint64_t programId) const;

    static std::string Duration(uint64_t seconds);
    static std::string Date(int64_t time);
    static std::string Ago(int64_t time);

private:
    PlayTime(const PlayTime &) = delete;
    PlayTime & operator=(const PlayTime &) = delete;

    void Load();
    void Save();

    const std::filesystem::path m_file;
    std::unordered_map<uint64_t, Record> m_records;
    uint64_t m_playing = 0;
    std::chrono::steady_clock::time_point m_since;
    std::chrono::steady_clock::time_point m_lastSaved;
    bool m_dirty = false;
};
