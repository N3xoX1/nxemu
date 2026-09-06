#pragma once

#include <memory>
#include <mutex>

#include <nxemu-core/modules/system_modules.h>
#include "android_render_window.h"

class EmulationSession
{
public:
    static EmulationSession & GetInstance();

    void InitializeSystem();
    void ShutdownSystem();

private:
    EmulationSession() = default;

    mutable std::mutex m_mutex;
    AndroidRenderWindow m_render_window;
    std::unique_ptr<SystemModules> m_system_modules;
};
