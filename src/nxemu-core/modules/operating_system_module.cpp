#include "operating_system_module.h"

OperatingSystemModule::OperatingSystemModule() :
    m_CreateOS(dummyCreateOS),
    m_DestroyOS(dummyDestroyOS)
{
}

void OperatingSystemModule::UnloadModule()
{
}

bool OperatingSystemModule::LoadFunctions()
{
    // Optional export: older OS modules keep their previous shutdown behavior.
    m_PrepareEmulationStopping = reinterpret_cast<tyPrepareEmulationStopping>(
        DynamicLibraryGetProc(m_lib, "PrepareEmulationStopping"));
    m_CreateOS = (tyCreateOperatingSystem)DynamicLibraryGetProc(m_lib, "CreateOperatingSystem");
    m_DestroyOS = (tyDestroyOperatingSystem)DynamicLibraryGetProc(m_lib, "DestroyOperatingSystem");

    bool res = true;
    if (m_CreateOS == nullptr)
    {
        m_CreateOS = dummyCreateOS;
        res = false;
    }
    if (m_DestroyOS == nullptr)
    {
        m_DestroyOS = dummyDestroyOS;
        res = false;
    }
    return res;
}

MODULE_TYPE OperatingSystemModule::ModuleType() const
{
    return MODULE_TYPE_OPERATING_SYSTEM;
}

IOperatingSystem * OperatingSystemModule::dummyCreateOS(ISystemModules & /*modules*/)
{
    return nullptr;
}

void OperatingSystemModule::dummyDestroyOS(IOperatingSystem * /*Video*/)
{
}
