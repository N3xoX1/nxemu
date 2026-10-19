#include "discord_presence.h"
#include <chrono>
#include <discord_rpc.h>

namespace
{
constexpr const char * kApplicationId = "1525639969828376796";
}

DiscordPresence::DiscordPresence() :
    m_enabled(false),
    m_initialized(false),
    m_startTimestamp(0)
{
}

DiscordPresence::~DiscordPresence()
{
    Shutdown();
}

void DiscordPresence::SetEnabled(bool enabled)
{
    if (m_enabled == enabled)
    {
        return;
    }
    m_enabled = enabled;
    if (m_enabled)
    {
        Initialize();
    }
    else
    {
        Shutdown();
    }
}

void DiscordPresence::Initialize()
{
    if (m_initialized)
    {
        return;
    }
    DiscordEventHandlers handlers{};
    Discord_Initialize(kApplicationId, &handlers, 1, nullptr);
    m_initialized = true;
}

void DiscordPresence::Shutdown()
{
    if (!m_initialized)
    {
        return;
    }
    Discord_ClearPresence();
    Discord_Shutdown();
    m_initialized = false;
    m_startTimestamp = 0;
}

void DiscordPresence::Clear()
{
    if (m_initialized)
    {
        Discord_ClearPresence();
    }
    m_startTimestamp = 0;
}

void DiscordPresence::Update(bool emulationRunning, const std::string & gameName)
{
    if (!m_enabled)
    {
        return;
    }
    Initialize();
    if (!m_initialized)
    {
        return;
    }

    DiscordRichPresence presence{};
    if (emulationRunning)
    {
        if (m_startTimestamp == 0)
        {
            m_startTimestamp = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
        presence.details = "Playing";
        presence.state = gameName.empty() ? "Unknown game" : gameName.c_str();
        presence.startTimestamp = m_startTimestamp;
    }
    else
    {
        m_startTimestamp = 0;
        presence.details = "In menu";
    }
    Discord_UpdatePresence(&presence);
}
