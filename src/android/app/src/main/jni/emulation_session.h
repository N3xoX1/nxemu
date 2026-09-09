#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <nxemu-core/modules/system_modules.h>
#include "android_render_window.h"

struct ANativeWindow;

class EmulationSession
{
public:
    static EmulationSession & GetInstance();

    void InitializeSystem();
    void ShutdownSystem();

    std::string QueryRomMetadata(const std::string & path);

    bool Run(ANativeWindow * native_window, float pixel_ratio, const std::string & rom_path);
    void SurfaceDestroyed();
    void SurfaceChanged();

    ANativeWindow * NativeWindow() const;
    float PixelRatio() const;

private:
    EmulationSession() = default;

    void RestoreStubModulesLocked();

    mutable std::mutex m_mutex;
    AndroidRenderWindow m_render_window;
    std::unique_ptr<SystemModules> m_system_modules;
    ANativeWindow * m_native_window = nullptr;
    float m_pixel_ratio = 1.0f;
};
