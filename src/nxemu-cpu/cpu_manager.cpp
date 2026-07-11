#include "cpu_manager.h"
#include "cpu_settings.h"
#include <nxemu-cpu/cpu_settings_identifiers.h>
#include <nxemu-module-spec/base.h>
#include "arm_dynarmic_64.h"
#include "arm_dynarmic_32.h"
#include "patch/patch_collection.h"
#if defined(ARCHITECTURE_x86_64) || defined(ARCHITECTURE_arm64)
#include "exclusive_monitor_interface.h"
#endif

extern IModuleSettings * g_settings;

CpuInterface::CpuInterface(ISystemModules & modules, uint32_t processorCount) :
    m_modules(modules),
    m_monitor(processorCount)
{
}

CpuInterface::~CpuInterface()
{
}

bool CpuInterface::Initialize(void)
{
    return true;
}

IExclusiveMonitor * CpuInterface::CreateExclusiveMonitor(IMemory & memory)
{
#if defined(ARCHITECTURE_x86_64) || defined(ARCHITECTURE_arm64)
    return new ExclusiveMonitor(memory, m_monitor);
#else
    // TODO(merry): Passthrough exclusive monitor
    return nullptr;
#endif
}

IPatchCollection * CpuInterface::CreatePatchCollection(bool is_application)
{
    return new PatchCollection(m_modules, is_application);
}

ICpuCore * CpuInterface::CreateCpuCore(ICoreSystem & system, bool is64Bit, bool usesWallClock, IKernelProcess & process, uint32_t coreIndex)
{
#ifdef HAS_NCE
    if (this->IsApplication() && g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        // Register the scoped JIT handler before creating any NCE instances
        // so that its signal handler will appear first in the signal chain.
        Core::ScopedJitExecution::RegisterHandler();

        return new Core::ArmNce > (system, true, coreIndex);
    }
    else
#endif
    if (is64Bit)
    {
        return new ArmDynarmic64(system, usesWallClock, process, m_monitor, coreIndex);
    }
    return new ArmDynarmic32(system, usesWallClock, process, m_monitor, coreIndex);
}
