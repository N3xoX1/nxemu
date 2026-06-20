#include "os_manager.h"
#include "os_settings.h"
#include <memory>
#include <stdio.h>
#include <nxemu-core/settings/identifiers.h>
#include <yuzu_common/fs/path_util.h>
#include <yuzu_common/logging/backend.h>
#ifdef ANDROID
#include <yuzu_common/android/java_bridge.h>
#include <yuzu_common/fs/fs_android.h>
#endif

std::unique_ptr<OSManager> g_osManager;
IModuleNotification * g_notify = nullptr;
IModuleSettings * g_settings = nullptr;

void LoggingSettingChanged(const char* setting, void* /*userData*/)
{
    if (strcmp(setting, NXCoreSetting::LogFilter) == 0)
    {
        Common::Log::ResetFilter(g_settings->GetString(NXCoreSetting::LogFilter));
    }
}

/*
Function: GetModuleInfo
Purpose: Fills the MODULE_INFO structure with information about the DLL.
Input: A pointer to a MODULE_INFO structure to be populated.
Output: none
*/
void CALL GetModuleInfo(MODULE_INFO * info)
{
    info->version = MODULE_OPERATING_SYSTEM_SPECS_VERSION;
    info->type = MODULE_TYPE_OPERATING_SYSTEM;
#ifdef _DEBUG
    sprintf(info->name, "NxEmu Operating System Plugin (Debug)");
#else
    sprintf(info->name, "NxEmu Operating System Plugin");
#endif
}

/*
Function: ModuleInitialize
Purpose: Initializes the module for global use.
Input: None
Output: Returns 0 on success
*/
int CALL ModuleInitialize(ModuleInterfaces & interfaces)
{
    g_notify = interfaces.notification;
    g_settings = interfaces.settings;

    if (g_notify == nullptr || g_settings == nullptr || interfaces.logger == nullptr)
    {
        return -1;
    }
    Common::FS::SetAppDirectory(std::string(g_settings->GetString(NXCoreSetting::AppDirectory)));
    Common::Log::Initialize(interfaces.logger, g_settings->GetString(NXCoreSetting::LogFilter));
    g_settings->RegisterCallback(NXCoreSetting::LogFilter, LoggingSettingChanged, nullptr);
#ifdef ANDROID
    SetJavaVM((JavaVM *)interfaces.java_vm);
    Common::FS::Android::RegisterModuleFsCallbacks((JavaVM *)interfaces.java_vm, interfaces.native_library_class);
#endif
    return 0;
}

/*
Function: ModuleCleanup
Purpose: Cleans up global resources used by the module.
Input: None
Output: None
*/
void CALL ModuleCleanup()
{
    if (g_osManager)
    {
        g_osManager->ShutdownMainProcess();
    }
}

/*
Function: EmulationStarting
Purpose: Called when emulation is starting
Input: None.
Output: None.
*/
void CALL EmulationStarting()
{
    g_osManager->EmulationStarting();
}

/*
Function: EmulationStopping
Purpose: Called when emulation is stopping
Input: None
Output: None
*/
void CALL EmulationStopping(bool wait)
{
    g_osManager->EmulationStopping(wait);
}

/*
Function: FlushSettings
Purpose: Called when emulation is saving settings
Input: None
Output: None
*/
EXPORT void CALL FlushSettings()
{
    SaveOsSettings();
}

IOperatingSystem * CALL CreateOperatingSystem(ISystemModules & modules)
{
    if (g_osManager.get() != nullptr)
    {
        g_notify->BreakPoint(__FILE__, __LINE__);
        return nullptr;
    }
    g_osManager = std::make_unique<OSManager>(modules);
    return g_osManager.get();
}

void CALL DestroyOperatingSystem(IOperatingSystem * operatingSystem)
{
    if (operatingSystem == nullptr || g_osManager.get() != operatingSystem)
    {
        g_notify->BreakPoint(__FILE__, __LINE__);
        return;
    }
    g_osManager = nullptr;
}

#ifdef _WIN32
extern "C" int __stdcall DllMain(void * /*hinst*/, unsigned long /*fdwReason*/, void * /*lpReserved*/)
{
    return true;
}
#endif
