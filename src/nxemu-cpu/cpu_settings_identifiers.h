#pragma once

namespace NXCpuSetting
{
    constexpr const char * SystemCpuBackend = "nxcpu:SystemCpuBackend";
    constexpr const char * SystemCpuAccuracy = "nxcpu:SystemCpuAccuracy";
    constexpr const char * GameCpuBackend = "nxcpu:GameCpuBackend";
    constexpr const char * GameCpuAccuracy = "nxcpu:GameCpuAccuracy";
    constexpr const char * CpuDebugMode = "nxcpu:CpuDebugMode";
    constexpr const char * CpuoptPageTables = "nxcpu:CpuoptPageTables";
    constexpr const char * CpuoptBlockLinking = "nxcpu:CpuoptBlockLinking";
    constexpr const char * CpuoptReturnStackBuffer = "nxcpu:CpuoptReturnStackBuffer";
    constexpr const char * CpuoptFastDispatcher = "nxcpu:CpuoptFastDispatcher";
    constexpr const char * CpuoptContextElimination = "nxcpu:CpuoptContextElimination";
    constexpr const char * CpuoptConstProp = "nxcpu:CpuoptConstProp";
    constexpr const char * CpuoptMiscIr = "nxcpu:CpuoptMiscIr";
    constexpr const char * CpuoptReduceMisalignChecks = "nxcpu:CpuoptReduceMisalignChecks";
    constexpr const char * CpuoptFastmem = "nxcpu:CpuoptFastmem";
    constexpr const char * CpuoptFastmemExclusives = "nxcpu:CpuoptFastmemExclusives";
    constexpr const char * CpuoptRecompileExclusives = "nxcpu:CpuoptRecompileExclusives";
    constexpr const char * CpuoptIgnoreMemoryAborts = "nxcpu:CpuoptIgnoreMemoryAborts";
    constexpr const char * CpuoptUnsafeUnfuseFma = "nxcpu:CpuoptUnsafeUnfuseFma";
    constexpr const char * CpuoptUnsafeReduceFpError = "nxcpu:CpuoptUnsafeReduceFpError";
    constexpr const char * CpuoptUnsafeIgnoreStandardFpcr = "nxcpu:CpuoptUnsafeIgnoreStandardFpcr";
    constexpr const char * CpuoptUnsafeInaccurateNan = "nxcpu:CpuoptUnsafeInaccurateNan";
    constexpr const char * CpuoptUnsafeFastmemCheck = "nxcpu:CpuoptUnsafeFastmemCheck";
    constexpr const char * CpuoptUnsafeIgnoreGlobalMonitor = "nxcpu:CpuoptUnsafeIgnoreGlobalMonitor";
    constexpr const char * NceEnabled = "nxcpu:NceEnabled";

} // namespace NXCpuSetting
