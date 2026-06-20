#include "module_base.h"
#include <yuzu_common/logging/backend.h>
#ifdef ANDROID
#include <yuzu_common/android/java_bridge.h>
#endif

ModuleBase::ModuleBase() :
    m_lib(nullptr),
    m_moduleInfo({0}),
    EmulationStarting(nullptr),
    EmulationStopping(nullptr),
    ModuleCleanup(nullptr),
    FlushSettings(nullptr)
{
}

ModuleBase::~ModuleBase()
{
    ModuleDone(false);
}

bool ModuleBase::Load(const char * fileName, IModuleNotification * notification, IModuleSettings * settings)
{
    ModuleDone();
    m_lib = DynamicLibraryOpen(fileName);
    if (m_lib == nullptr)
    {
        return false;
    }

    ModuleBase::tyGetModuleInfo GetModuleInfo = (ModuleBase::tyGetModuleInfo)DynamicLibraryGetProc(m_lib, "GetModuleInfo");
    if (GetModuleInfo == nullptr)
    {
        return false;
    }

    GetModuleInfo(&m_moduleInfo);
    if (!ValidVersion(m_moduleInfo))
    {
        return false;
    }
    if (m_moduleInfo.type != ModuleType())
    {
        return false;
    }

    ModuleBase::tyModuleInitialize ModuleInitialize = (ModuleBase::tyModuleInitialize)DynamicLibraryGetProc(m_lib, "ModuleInitialize");
    ModuleCleanup = (ModuleBase::tyModuleCleanup)DynamicLibraryGetProc(m_lib, "ModuleCleanup");
    EmulationStarting = (ModuleBase::tyEmulationStarting)DynamicLibraryGetProc(m_lib, "EmulationStarting");
    EmulationStopping = (ModuleBase::tyEmulationStopping)DynamicLibraryGetProc(m_lib, "EmulationStopping");
    FlushSettings = (ModuleBase::tyFlushSettings)DynamicLibraryGetProc(m_lib, "FlushSettings");

    if (ModuleInitialize == nullptr ||
        ModuleCleanup == nullptr ||
        EmulationStarting == nullptr ||
        EmulationStopping == nullptr ||
        FlushSettings == nullptr)
    {
        return false;
    }

    if (!LoadFunctions())
    {
        return false;
    }

    ModuleInterfaces interfaces = {0};
    interfaces.notification = notification;
    interfaces.settings = settings;
    interfaces.logger = Common::Log::ModuleLogger();
#ifdef ANDROID
    interfaces.java_vm = GetJavaVM();
    interfaces.native_library_class = GetNativeLibraryGlobalRef();
#else
    interfaces.java_vm = nullptr;
    interfaces.native_library_class = nullptr;
#endif
    if (ModuleInitialize(interfaces) != 0)
    {
        return false;
    }
    return true;
}

bool ModuleBase::ValidVersion(MODULE_INFO & info)
{
    if (info.type == MODULE_TYPE_LOADER && info.version == MODULE_LOADER_SPECS_VERSION)
    {
        return true;
    }
    if (info.type == MODULE_TYPE_CPU && info.version == MODULE_CPU_SPECS_VERSION)
    {
        return true;
    }
    if (info.type == MODULE_TYPE_VIDEO && info.version == MODULE_VIDEO_SPECS_VERSION)
    {
        return true;
    }
    if (info.type == MODULE_TYPE_OPERATING_SYSTEM && info.version == MODULE_OPERATING_SYSTEM_SPECS_VERSION)
    {
        return true;
    }
    return false;
}

void ModuleBase::ModuleDone(bool callUnloadModule)
{
    if (m_lib == nullptr)
    {
        return;
    }
    if (ModuleCleanup != nullptr)
    {
        ModuleCleanup();    
    }
    if (callUnloadModule)
    {
        UnloadModule();
    }
    DynamicLibraryClose(m_lib);
    m_lib = nullptr;
    ModuleCleanup = nullptr;
    EmulationStarting = nullptr;
    EmulationStopping = nullptr;
    FlushSettings = nullptr;
}