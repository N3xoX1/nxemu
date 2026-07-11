#pragma once
#include "base.h"
#include <stdint.h>

enum class CpuBackend : uint32_t
{
    Dynarmic = 0,
    Nce,
};

enum class CpuAccuracy : uint32_t
{
    Auto = 0,
    Accurate,
    Unsafe,
    Paranoid,
};

enum class CpuSettingTranslationType : uint32_t
{
    CpuBackend = 0,
    CpuAccuracy,
};

nxinterface ICpuCore;

enum class CpuDebugWatchpointType : uint8_t
{
    None = 0,
    Read = 1 << 0,
    Write = 1 << 1,
    ReadOrWrite = Read | Write,
};

enum class ProcessorArchitecture
{
    AArch64,
    AArch32,
};

enum class CpuHaltReason
{
    StepThread,
    DataAbort,
    BreakLoop,
    SupervisorCall,
    SupervisorCallBreakLoop,
    InstructionBreakpoint,
    PrefetchAbort,
    PrefetchAbortBreakLoop,
};

struct CpuDebugWatchpoint
{
    uint64_t startAddress;
    uint64_t endAddress;
    CpuDebugWatchpointType type;
};

nxinterface ICoreTiming
{
    virtual void AddTicks(uint64_t ticks) = 0;
    virtual int64_t GetDowncount() const = 0;
    virtual uint64_t GetClockTicks() const = 0;
};

nxinterface ICoreSystem
{
    virtual bool DebuggerEnabled() const = 0;
    virtual ICoreTiming & Timing() = 0;
};

nxinterface IMemory
{
    virtual bool IsValidVirtualAddressRange(uint64_t base, uint64_t size) const = 0;
    virtual void RasterizerMarkRegionCached(uint64_t vaddr, uint64_t size, bool cached) = 0;
    virtual uint8_t * GetPointerSilent(uint64_t vaddr) = 0;

    virtual uint8_t Read8(uint64_t addr) = 0;
    virtual uint16_t Read16(uint64_t addr) = 0;
    virtual uint32_t Read32(uint64_t addr) = 0;
    virtual uint64_t Read64(uint64_t addr) = 0;
    virtual bool ReadBlock(uint64_t src_addr, void * dest_buffer, uint64_t size) = 0;

    virtual void Write8(uint64_t addr, uint8_t value) = 0;
    virtual void Write16(uint64_t addr, uint16_t value) = 0;
    virtual void Write32(uint64_t addr, uint32_t value) = 0;
    virtual void Write64(uint64_t addr, uint64_t value) = 0;

    virtual bool WriteExclusive8(uint64_t addr, uint8_t value, uint8_t expected) = 0;
    virtual bool WriteExclusive16(uint64_t addr, uint16_t value, uint16_t expected) = 0;
    virtual bool WriteExclusive32(uint64_t addr, uint32_t value, uint32_t expected) = 0;
    virtual bool WriteExclusive64(uint64_t addr, uint64_t value, uint64_t expected) = 0;
    virtual bool WriteExclusive128(uint64_t addr, uint64_t valueHi, uint64_t valueLow, uint64_t expectedHi, uint64_t expectedLow) = 0;
};

nxinterface IExclusiveMonitor
{
    virtual uint8_t ExclusiveRead8(uint32_t coreIndex, uint64_t addr) = 0;
    virtual uint16_t ExclusiveRead16(uint32_t coreIndex, uint64_t addr) = 0;
    virtual uint32_t ExclusiveRead32(uint32_t coreIndex, uint64_t addr) = 0;
    virtual uint64_t ExclusiveRead64(uint32_t coreIndex, uint64_t addr) = 0;
    virtual void ExclusiveRead128(uint32_t coreIndex, uint64_t addr, uint64_t & outHigh, uint64_t & outLow) = 0;
    virtual void ClearExclusive(uint32_t coreIndex) = 0;

    virtual bool ExclusiveWrite8(uint32_t coreIndex, uint64_t addr, uint8_t value) = 0;
    virtual bool ExclusiveWrite16(uint32_t coreIndex, uint64_t addr, uint16_t value) = 0;
    virtual bool ExclusiveWrite32(uint32_t coreIndex, uint64_t addr, uint32_t value) = 0;
    virtual bool ExclusiveWrite64(uint32_t coreIndex, uint64_t addr, uint64_t value) = 0;
    virtual bool ExclusiveWrite128(uint32_t coreIndex, uint64_t addr, uint64_t valueHigh, uint64_t valueLow) = 0;

    virtual void Release() = 0;
};

nxinterface IKProcessPageTable
{
    virtual uint32_t GetAddressSpaceWidth() const = 0;
    virtual uint8_t * FastmemArena() const = 0;
    virtual void ** PageTable() const = 0;
};

nxinterface IKernelProcess
{
    virtual IKProcessPageTable & GetPageTable() = 0;
    virtual IMemory & GetMemory() = 0;
    virtual bool Is64Bit() const = 0;
    virtual void LogBacktrace(ICpuCore & cpuCore) = 0;
};

nxinterface IKernelThread
{
    virtual IKernelProcess * GetOwnerProcess() const = 0;
};

struct CpuThreadContext
{
    uint64_t r[29];
    uint64_t fp;
    uint64_t lr;
    uint64_t sp;
    uint64_t pc;
    uint32_t pstate;
    uint32_t padding;
    uint64_t v[32][2];
    uint32_t fpcr;
    uint32_t fpsr;
    uint64_t tpidr;
};
static_assert(sizeof(CpuThreadContext) == 0x320);

nxinterface IPatchCollection
{
    virtual void PatchText(int32_t patch_index, const uint8_t * program_image, uint32_t image_size, uint32_t code_offset, uint32_t code_size) = 0;
    virtual void Relocate(int32_t patch_index, uint64_t load_base, uint8_t * program_image, uint32_t * image_size, uint32_t code_offset, uint32_t code_size, uint64_t * segment_addr, uint32_t * segment_size) = 0;
    virtual uint32_t GetTotalPatchSize() const = 0;
    virtual uint32_t GetPreTextSize(int32_t patch_index) const = 0;
    virtual int32_t GetLastIndex() const = 0;
    virtual void SaveIndex(uint32_t module_index) = 0;
    virtual int32_t GetIndex(uint32_t module_index) const = 0;
    virtual void Release() = 0;
};

nxinterface ICpuCore
{
    virtual void Initialize() = 0;
    virtual ProcessorArchitecture GetArchitecture() const = 0;
    virtual uint32_t GetSvcNumber() const = 0;
    virtual void GetContext(CpuThreadContext & ctx) const = 0;
    virtual void SetContext(const CpuThreadContext & ctx) = 0;
    virtual void GetSvcArguments(uint64_t (&args)[8]) const = 0;
    virtual void SetSvcArguments(const uint64_t (&args)[8]) = 0;
    virtual void SetTpidrroEl0(uint64_t value) = 0;
    virtual CpuHaltReason RunThread(IKernelThread * thread) = 0;
    virtual CpuHaltReason StepThread(IKernelThread * thread) = 0;
    virtual void LockThread(IKernelThread * thread) = 0;
    virtual void UnlockThread(IKernelThread * thread) = 0;
    virtual void InvalidateCacheRange(uint64_t addr, uint64_t size) = 0;
    virtual const CpuDebugWatchpoint * HaltedWatchpoint() const = 0;
    virtual void RewindBreakpointInstruction() = 0;
    virtual void SetWatchpointArray(const CpuDebugWatchpoint * watchpoints, uint32_t count) = 0;
    virtual void SignalInterrupt(IKernelThread * thread) = 0;
    
    virtual void Release() = 0;
};

nxinterface ICpu
{
    virtual bool Initialize(void) = 0;

    virtual IExclusiveMonitor * CreateExclusiveMonitor(IMemory & memory) = 0;
    virtual IPatchCollection * CreatePatchCollection(bool is_application) = 0;
    virtual ICpuCore * CreateCpuCore(ICoreSystem & system, bool is64Bit, bool usesWallClock, IKernelProcess & process, uint32_t coreIndex) = 0;
};

EXPORT ICpu * CALL CreateCpu(ISystemModules & modules);
EXPORT void CALL DestroyCpu(ICpu * Cpu);
