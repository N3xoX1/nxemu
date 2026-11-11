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

enum class GpuInitializationState : int32_t
{
    Preparing,
    Ready,
    Failed,
    // A shader import session: shaders are imported and built, the game never runs.
    Finished,
};

namespace NXCoreSetting
{
constexpr const char * AppBaseDirectory = "nxcore:AppBaseDirectory";
constexpr const char * AppDirectory = "nxcore:AppDirectory";
constexpr const char * ConfigFile = "nxcore:ConfigFile";
constexpr const char * GameFile = "nxcore:GameFile";
constexpr const char * GameName = "nxcore:GameName";
constexpr const char * GameTitleId = "nxcore:GameTitleId";
constexpr const char * ModuleDirectory = "nxcore:ModuleDirectory";
constexpr const char * ModuleLoader = "nxcore:ModuleLoader";
constexpr const char * ModuleCpu = "nxcore:ModuleCpu";
constexpr const char * ModuleVideo = "nxcore:ModuleVideo";
constexpr const char * ModuleOs = "nxcore:ModuleOs";
constexpr const char * ShowLogConsole = "nxcore:ShowLogConsole";
constexpr const char * LogFilter = "nxcore:LogFilter";
constexpr const char * DebugSettingsEnabled = "nxcore:DebugSettingsEnabled";
constexpr const char * EmulationRunning = "nxcore:EmulationRunning";
constexpr const char * EmulationState = "nxcore:EmulationState";
constexpr const char * GameLoadResult = "nxcore:GameLoadResult";
constexpr const char * Has39BitAddressSpace = "nxcore:Has39BitAddressSpace";
constexpr const char * Has36BitAddressSpace = "nxcore:Has36BitAddressSpace";
constexpr const char * Is64BitProgram = "nxcore:Is64BitProgram";
constexpr const char * DisplayedFrames = "nxcore:DisplayedFrames";
// Runtime-only readiness state; never a configurable user preference.
constexpr const char * GpuInitializationState = "nxcore:GpuInitializationState";
constexpr const char * ShuttingDown = "nxcore:ShuttingDown";
constexpr const char * DiskCacheLoadStage = "nxcore:DiskCacheLoadStage";
constexpr const char * DiskCacheLoadCurrent = "nxcore:DiskCacheLoadCurrent";
constexpr const char * DiskCacheLoadTotal = "nxcore:DiskCacheLoadTotal";
constexpr const char * DiskCacheLoadTick = "nxcore:DiskCacheLoadTick";
// Runtime-only: a .nxshader file to import while the game loads, then stop before it runs,
// and the reason the import failed (empty when it succeeded).
constexpr const char * ShaderImportFile = "nxcore:ShaderImportFile";
constexpr const char * ShaderImportError = "nxcore:ShaderImportError";
// Runtime-only: true while ExeFS patches or cheats keep this run from recording shaders.
constexpr const char * ShaderRecordingOff = "nxcore:ShaderRecordingOff";
constexpr const char * GpuHookLibDir = "nxcore:GpuHookLibDir";
constexpr const char * GpuCustomDriverDir = "nxcore:GpuCustomDriverDir";
constexpr const char * GpuCustomDriverName = "nxcore:GpuCustomDriverName";
constexpr const char * GpuFileRedirectDir = "nxcore:GpuFileRedirectDir";
constexpr const char * SystemLanguageIndex = "nxcore:SystemLanguageIndex";

} // namespace NXCoreSetting