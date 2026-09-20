#include "cpu_manager.h"
#include "arm_dynarmic_64.h"
#include "arm_dynarmic_32.h"
#include "patch/patch_collection.h"
#if defined(_M_X64) || defined(ARCHITECTURE_x86_64) || defined(_M_ARM64) || defined(ARCHITECTURE_arm64)
#include "exclusive_monitor_interface.h"
#endif
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
#include "cpu_settings_identifiers.h"
#include "nce/arm_nce.h"
#include <nxemu-module-spec/base.h>

extern IModuleSettings * g_settings;
#endif

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
#if defined(_M_X64) || defined(ARCHITECTURE_x86_64) || defined(_M_ARM64) || defined(ARCHITECTURE_arm64)
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
#if defined(_M_ARM64) || defined(ARCHITECTURE_arm64) || defined(__aarch64__)
    if (is64Bit && g_settings && g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        return new Core::ArmNce(system, usesWallClock, process, coreIndex);
    }
#endif
    if (is64Bit)
    {
        return new ArmDynarmic64(system, usesWallClock, process, m_monitor, coreIndex);
    }
    return new ArmDynarmic32(system, usesWallClock, process, m_monitor, coreIndex);
}
