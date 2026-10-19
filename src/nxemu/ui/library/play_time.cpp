#include "play_time.h"
#include <algorithm>
#include <cinttypes>
#include <common/json.h>
#include <common/std_string.h>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <nxemu/ui/i18n/translation.h>
#include <system_error>

namespace
{
constexpr auto kSaveInterval = std::chrono::minutes(1);
constexpr uintmax_t kMaxFileBytes = 8 * 1024 * 1024;

int64_t Now()
{
    return static_cast<int64_t>(std::time(nullptr));
}
}

PlayTime::PlayTime(std::filesystem::path file) :
    m_file(std::move(file))
{
    Load();
}

PlayTime::~PlayTime()
{
    SetPlaying(0);
}

void PlayTime::SetPlaying(uint64_t programId)
{
    const auto now = std::chrono::steady_clock::now();
    if (m_playing != 0)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_since);
        if (elapsed.count() > 0)
        {
            Record & record = m_records[m_playing];
            record.seconds += static_cast<uint64_t>(elapsed.count());
            record.lastPlayed = Now();
            m_since += elapsed;
            m_dirty = true;
        }
    }
    if (programId != m_playing)
    {
        m_playing = programId;
        m_since = now;
        if (programId != 0)
        {
            m_records[programId].lastPlayed = Now();
            m_dirty = true;
        }
        Save();
    }
    else if (now - m_lastSaved >= kSaveInterval)
    {
        Save();
    }
}

PlayTime::Record PlayTime::Of(uint64_t programId) const
{
    const auto record = m_records.find(programId);
    return record != m_records.end() ? record->second : Record();
}

std::string PlayTime::Duration(uint64_t seconds)
{
    if (seconds == 0)
    {
        return {};
    }
    const uint64_t minutes = seconds / 60;
    if (minutes == 0)
    {
        return Tr("< 1m");
    }
    return minutes < 60 ? Tr("%1m", {std::to_string(minutes)}) : Tr("%1h %2m", {std::to_string(minutes / 60), std::to_string(minutes % 60)});
}

std::string PlayTime::Date(int64_t time)
{
    if (time == 0)
    {
        return {};
    }
    const std::time_t value = static_cast<std::time_t>(time);
    const std::tm * local = std::localtime(&value);
    char text[64];
    const std::string format = Tr("%b %d, %Y  %H:%M");
    return local != nullptr && std::strftime(text, sizeof(text), format.c_str(), local) != 0 ? text : std::string();
}

std::string PlayTime::Ago(int64_t time)
{
    if (time == 0)
    {
        return {};
    }
    const int64_t minutes = std::max<int64_t>(Now() - time, 0) / 60;
    if (minutes < 1)
    {
        return Tr("Just now");
    }
    if (minutes < 60)
    {
        return TrCount("%n minute ago", "%n minutes ago", static_cast<uint64_t>(minutes));
    }
    const int64_t hours = minutes / 60;
    if (hours < 24)
    {
        return TrCount("%n hour ago", "%n hours ago", static_cast<uint64_t>(hours));
    }
    const int64_t days = hours / 24;
    if (days < 14)
    {
        return TrCount("%n day ago", "%n days ago", static_cast<uint64_t>(days));
    }
    if (days < 60)
    {
        return TrCount("%n week ago", "%n weeks ago", static_cast<uint64_t>(days / 7));
    }
    return days < 365 ? TrCount("%n month ago", "%n months ago", static_cast<uint64_t>(days / 30)) : TrCount("%n year ago", "%n years ago", static_cast<uint64_t>(days / 365));
}

void PlayTime::Load()
{
    std::error_code error;
    const uintmax_t bytes = std::filesystem::file_size(m_file, error);
    if (error || bytes > kMaxFileBytes)
    {
        return;
    }
    std::ifstream input(m_file, std::ios::binary);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    JsonValue root;
    if (!JsonReader().Parse(contents.data(), contents.data() + contents.size(), root) || !root.isObject() || !root["games"].isArray())
    {
        return;
    }
    const JsonValue & games = root["games"];
    for (uint32_t i = 0; i < games.size(); i++)
    {
        const JsonValue & item = games[i];
        uint64_t programId = 0;
        if (!item.isObject() || !item["application"].isString() ||
            std::sscanf(item["application"].asString().c_str(), "%16" SCNx64, &programId) != 1 || programId == 0)
        {
            continue;
        }
        Record record;
        record.seconds = item["seconds"].isInt() ? item["seconds"].asUInt64() : 0;
        record.lastPlayed = item["lastPlayed"].isInt() ? item["lastPlayed"].asInt64() : 0;
        m_records[programId] = record;
    }
}

void PlayTime::Save()
{
    m_lastSaved = std::chrono::steady_clock::now();
    if (!m_dirty)
    {
        return;
    }
    JsonValue games(JsonValueType::Array);
    for (const auto & [programId, record] : m_records)
    {
        JsonValue item(JsonValueType::Object);
        item["application"] = std::string(stdstr_f("%016llX", static_cast<unsigned long long>(programId)));
        item["seconds"] = static_cast<int64_t>(record.seconds);
        item["lastPlayed"] = record.lastPlayed;
        games.Append(std::move(item));
    }
    JsonValue root(JsonValueType::Object);
    root["games"] = std::move(games);
    const std::string contents = JsonStyledWriter().write(root);

    std::filesystem::path temporary = m_file;
    temporary += ".tmp";
    std::error_code error;
    std::filesystem::create_directories(m_file.parent_path(), error);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output)
        {
            output.close();
            std::filesystem::remove(temporary, error);
            return;
        }
    }
    std::filesystem::rename(temporary, m_file, error);
    if (!error)
    {
        m_dirty = false;
    }
}
