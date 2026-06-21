#pragma once
#include <stdint.h>

enum class EmulationState : int32_t
{
    Stopped,
    Starting,
    LoadingRom,
    RomLoaded,
    Running,
    Paused,
    Stopping,
};

namespace NXCoreSetting
{
constexpr const char * AppBaseDirectory = "nxcore:AppBaseDirectory";
constexpr const char * AppDirectory = "nxcore:AppDirectory";
constexpr const char * ConfigFile = "nxcore:ConfigFile";
constexpr const char * GameFile = "nxcore:GameFile";
constexpr const char * GameName = "nxcore:GameName";
constexpr const char * ModuleDirectory = "nxcore:ModuleDirectory";
constexpr const char * ModuleLoader = "nxcore:ModuleLoader";
constexpr const char * ModuleCpu = "nxcore:ModuleCpu";
constexpr const char * ModuleVideo = "nxcore:ModuleVideo";
constexpr const char * ModuleOs = "nxcore:ModuleOs";
constexpr const char * ShowLogConsole = "nxcore:ShowLogConsole";
constexpr const char * LogFilter = "nxcore:LogFilter";
constexpr const char * EmulationRunning = "nxcore:EmulationRunning";
constexpr const char * EmulationState = "nxcore:EmulationState";
constexpr const char * DisplayedFrames = "nxcore:DisplayedFrames";
constexpr const char * ShuttingDown = "nxcore:ShuttingDown";
constexpr const char * DiskCacheLoadStage = "nxcore:DiskCacheLoadStage";
constexpr const char * DiskCacheLoadCurrent = "nxcore:DiskCacheLoadCurrent";
constexpr const char * DiskCacheLoadTotal = "nxcore:DiskCacheLoadTotal";
constexpr const char * DiskCacheLoadTick = "nxcore:DiskCacheLoadTick";
constexpr const char * GpuHookLibDir = "nxcore:GpuHookLibDir";
constexpr const char * GpuCustomDriverDir = "nxcore:GpuCustomDriverDir";
constexpr const char * GpuCustomDriverName = "nxcore:GpuCustomDriverName";
constexpr const char * GpuFileRedirectDir = "nxcore:GpuFileRedirectDir";

} // namespace NXCoreSetting