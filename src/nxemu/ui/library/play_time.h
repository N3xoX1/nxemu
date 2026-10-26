#pragma once
#include <cstdint>
#include <nxemu-core/play_time.h>
#include <string>

class PlayTime : public PlayTimeRecords
{
public:
    using PlayTimeRecords::PlayTimeRecords;

    static std::string Duration(uint64_t seconds);
    static std::string Date(int64_t time);
    static std::string Ago(int64_t time);
};
