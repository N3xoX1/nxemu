#include "cpu_manager.h"
#include "arm_dynarmic_64.h"
#include "arm_dynarmic_32.h"
#include "patch/patch_collection.h"
#include <yuzu_common/logging/log.h>
#if defined(_M_X64) || defined(ARCHITECTURE_x86_64) || defined(_M_ARM64) || defined(ARCHITECTURE_arm64)
#include "exclusive_monitor_interface.h"
#endif
#if defined(NXEMU_HAS_NCE) || defined(NXEMU_HAS_APPLE_HV)
#include "cpu_settings_identifiers.h"
#if defined(NXEMU_HAS_NCE)
#include "nce/arm_nce.h"
#endif
#include <nxemu-module-spec/base.h>

extern IModuleSettings * g_settings;
#endif

#if defined(NXEMU_HAS_APPLE_HV)
#include "apple_hv/apple_hv_address_space.h"
#include "apple_hv/arm_apple_hv.h"
#include <nxemu-module-spec/operating_system.h>
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

void CpuInterface::PrepareProcess(IKernelProcess & process, bool is64Bit, bool isApplication, bool usesWallClock)
{
#if defined(NXEMU_HAS_APPLE_HV)
    if (!is64Bit || !isApplication || !usesWallClock || !g_settings || !g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        return;
    }
    std::scoped_lock lock{m_apple_hv_spaces_mutex};
    if (m_apple_hv_spaces.contains(&process))
    {
        return;
    }
    const uint32_t address_bits = process.GetPageTable().GetAddressSpaceWidth();
    if (address_bits != 36 && address_bits != 39)
    {
        LOG_WARNING(Core_ARM, "AppleHv requires a 36-bit or 39-bit application address space (got {})", address_bits);
        return;
    }
    try
    {
        auto address_space = std::make_shared<Core::AppleHv::AddressSpace>(
            m_modules.OperatingSystem().DeviceMemory(), process.GetPageTable(), address_bits);
        if (address_space->IsValid())
        {
            m_apple_hv_spaces[&process] = std::move(address_space);
        }
    }
    catch (const std::exception & e)
    {
        LOG_ERROR(Core_ARM, "AppleHv initialization failed: {}. Falling back to Dynarmic", e.what());
    }
#else
    (void)process;
    (void)is64Bit;
    (void)isApplication;
    (void)usesWallClock;
#endif
}

void CpuInterface::FinalizeProcess(IKernelProcess & process)
{
#if defined(NXEMU_HAS_APPLE_HV)
    std::shared_ptr<Core::AppleHv::AddressSpace> address_space;
    {
        std::scoped_lock lock{m_apple_hv_spaces_mutex};
        const auto it = m_apple_hv_spaces.find(&process);
        if (it != m_apple_hv_spaces.end())
        {
            address_space = std::move(it->second);
            m_apple_hv_spaces.erase(it);
        }
    }
    if (address_space)
    {
        address_space->DetachPageTable();
    }
#else
    (void)process;
#endif
}

bool CpuInterface::RequiresDirectMemoryMapping() const
{
#if defined(NXEMU_HAS_NCE) && !defined(NXEMU_HAS_APPLE_HV)
    return true;
#else
    return false;
#endif
}

bool CpuInterface::IsNativeExecutionPrepared(IKernelProcess & process) const
{
#if defined(NXEMU_HAS_APPLE_HV)
    std::scoped_lock lock{m_apple_hv_spaces_mutex};
    const auto it = m_apple_hv_spaces.find(&process);
    return it != m_apple_hv_spaces.end() && it->second && it->second->IsValid();
#elif defined(NXEMU_HAS_NCE)
    (void)process;
    return g_settings && g_settings->GetBool(NXCpuSetting::NceEnabled);
#else
    (void)process;
    return false;
#endif
}

ICpuCore * CpuInterface::CreateCpuCore(ICoreSystem & system, bool is64Bit, bool usesWallClock, IKernelProcess & process, uint32_t coreIndex)
{
#if defined(NXEMU_HAS_APPLE_HV)
    if (is64Bit && process.IsApplication() && g_settings &&
        g_settings->GetBool(NXCpuSetting::NceEnabled))
    {
        std::shared_ptr<Core::AppleHv::AddressSpace> address_space;
        {
            std::scoped_lock lock{m_apple_hv_spaces_mutex};
            const auto it = m_apple_hv_spaces.find(&process);
            if (it != m_apple_hv_spaces.end())
            {
                address_space = it->second;
            }
        }
        if (address_space && address_space->IsValid())
        {
            return new Core::ArmAppleHv(system, process, std::move(address_space), coreIndex);
        }
    }
#elif defined(NXEMU_HAS_NCE)
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
