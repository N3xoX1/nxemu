#pragma once
#include "core/cpu_manager.h"
#include <atomic>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-module-spec/video.h>
struct IModuleSettings
{
    virtual ~IModuleSettings() = default;
    virtual void SetBool(const char *, bool) = 0;
    virtual void SetInt(const char *, int32_t) = 0;
};
namespace Core
{
class System
{
  public:
    StubSystemModules mods;
    CpuManager cpu;
    std::atomic<int> runs{0}, pauses{0}, shutdowns{0};
    void RegisterHostThread()
    {
    }
    StubSystemModules &GetSystemModules()
    {
        return mods;
    }
    CpuManager &GetCpuManager()
    {
        return cpu;
    }
    void Run()
    {
        runs++;
    }
    void Pause()
    {
        pauses++;
    }
    void ShutdownMainProcess()
    {
        shutdowns++;
    }
};
} // namespace Core
