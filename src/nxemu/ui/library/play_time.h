#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

// How long each game has been played, and when last, kept between runs in a
// file of the emulator's data. Games are told apart by their program id.
class PlayTime
{
public:
    struct Record
    {
        uint64_t seconds = 0;
        int64_t lastPlayed = 0; // seconds since the Unix epoch, 0 for never
    };

    explicit PlayTime(std::filesystem::path file);
    ~PlayTime();

    // Says which game is being played right now, 0 for none. Called every
    // frame: the time between two calls naming the same game is added to it.
    void SetPlaying(uint64_t programId);
    Record Of(uint64_t programId) const;

    // "3h 12m", "12m", "< 1m"; empty for a game that was never played.
    static std::string Duration(uint64_t seconds);
    // The day and time in the user's time zone; empty for never.
    static std::string Date(int64_t time);
    // How long ago that was, as "3 days ago"; empty for never.
    static std::string Ago(int64_t time);

private:
    PlayTime(const PlayTime &) = delete;
    PlayTime & operator=(const PlayTime &) = delete;

    void Load();
    void Save();

    const std::filesystem::path m_file;
    std::unordered_map<uint64_t, Record> m_records;
    uint64_t m_playing = 0;
    std::chrono::steady_clock::time_point m_since;     // start of the time not yet counted
    std::chrono::steady_clock::time_point m_lastSaved;
    bool m_dirty = false;
};
