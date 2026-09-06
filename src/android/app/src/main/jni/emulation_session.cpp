#include "emulation_session.h"

#include <android/log.h>
#include <cstdio>
#include <string_view>
#include <vector>

#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>

namespace
{
    constexpr const char * kLogTag = "NxEmu";
}

EmulationSession & EmulationSession::GetInstance()
{
    static EmulationSession instance;
    return instance;
}

void EmulationSession::InitializeSystem()
{
    std::lock_guard lock(m_mutex);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    m_system_modules.reset();
    m_system_modules = std::make_unique<SystemModules>();
    m_system_modules->Setup(m_render_window);
    if (m_system_modules->IsValid())
    {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "SystemModules: setup OK");
    }
    else
    {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "SystemModules: setup incomplete — ROM metadata will be unavailable");
        m_system_modules.reset();
    }
}

void EmulationSession::ShutdownSystem()
{
    std::lock_guard lock(m_mutex);
    m_system_modules.reset();
}
