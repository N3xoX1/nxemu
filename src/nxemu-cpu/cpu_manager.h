#pragma once
#include <nxemu-module-spec/cpu.h>
#include "dynarmic/interface/exclusive_monitor.h"

class CpuInterface :
    public ICpu
{
public:
    CpuInterface(ISystemModules & modules, uint32_t processorCount);
    ~CpuInterface();

    //ICpu
    bool Initialize() override;
    IExclusiveMonitor * CreateExclusiveMonitor(IMemory & memory) override;
    IPatchCollection * CreatePatchCollection(bool is_application) override;
    ICpuCore * CreateCpuCore(ICoreSystem & system, bool is64Bit, bool usesWallClock, IKernelProcess & process, uint32_t coreIndex) override;

private:
    CpuInterface() = delete;
    CpuInterface(const CpuInterface &) = delete;
    CpuInterface & operator=(const CpuInterface &) = delete;

    ISystemModules & m_modules;
    Dynarmic::ExclusiveMonitor m_monitor;
};
