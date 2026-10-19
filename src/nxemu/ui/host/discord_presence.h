#pragma once
#include <cstdint>
#include <string>

class DiscordPresence
{
public:
    DiscordPresence();
    ~DiscordPresence();

    void SetEnabled(bool enabled);
    void Update(bool emulationRunning, const std::string & gameName);

private:
    DiscordPresence(const DiscordPresence &) = delete;
    DiscordPresence & operator=(const DiscordPresence &) = delete;

    void Initialize();
    void Shutdown();
    void Clear();

    bool m_enabled;
    bool m_initialized;
    int64_t m_startTimestamp;
};
