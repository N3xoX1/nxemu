#pragma once
#include <nxemu-module-spec/cpu.h>
#include "dynarmic/interface/exclusive_monitor.h"
#if defined(NXEMU_HAS_APPLE_HV)
#include <memory>
#include <mutex>
#include <unordered_map>
namespace Core::AppleHv { class AddressSpace; }
#endif

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
    void PrepareProcess(IKernelProcess & process, bool is64Bit, bool isApplication, bool usesWallClock) override;
    void FinalizeProcess(IKernelProcess & process) override;
    bool RequiresDirectMemoryMapping() const override;
    bool IsNativeExecutionPrepared(IKernelProcess & process) const override;
    ICpuCore * CreateCpuCore(ICoreSystem & system, bool is64Bit, bool usesWallClock, IKernelProcess & process, uint32_t coreIndex) override;

private:
    CpuInterface() = delete;
    CpuInterface(const CpuInterface &) = delete;
    CpuInterface & operator=(const CpuInterface &) = delete;

    ISystemModules & m_modules;
    Dynarmic::ExclusiveMonitor m_monitor;
#if defined(NXEMU_HAS_APPLE_HV)
    mutable std::mutex m_apple_hv_spaces_mutex;
    std::unordered_map<IKernelProcess*, std::shared_ptr<Core::AppleHv::AddressSpace>> m_apple_hv_spaces;
#endif
};
