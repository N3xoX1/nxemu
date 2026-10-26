#include "play_time.h"
#include <algorithm>
#include <ctime>
#include <nxemu/ui/i18n/translation.h>

namespace
{
int64_t Now()
{
    return static_cast<int64_t>(std::time(nullptr));
}
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
