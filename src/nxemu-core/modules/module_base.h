#pragma once
#include <common/dynamic_library.h>

#ifndef EXPORT
#define EXPORT
#endif
#include <nxemu-module-spec/base.h>

class ModuleList;

class ModuleBase
{
    friend ModuleList;

    typedef void(CALL * tyGetModuleInfo)(MODULE_INFO * info);
    typedef int(CALL * tyModuleInitialize)(ModuleInterfaces & interfaces);
    typedef void(CALL * tyModuleCleanup)();
    typedef void(CALL * tyEmulationStarting)();
    typedef void(CALL * tyEmulationStopping)(bool wait);
    typedef void(CALL * tyFlushSettings)();

public:
    ModuleBase();
    ~ModuleBase();

    bool Load(const char * fileName, IModuleNotification * notification, IModuleSettings * settings);
    ModuleBase::tyEmulationStarting EmulationStarting;
    ModuleBase::tyEmulationStopping EmulationStopping;
    ModuleBase::tyModuleCleanup ModuleCleanup;
    ModuleBase::tyFlushSettings FlushSettings;

protected:
    virtual void UnloadModule(void) = 0;
    virtual bool LoadFunctions(void) = 0;
    virtual MODULE_TYPE ModuleType() const = 0;

    static bool ValidVersion(MODULE_INFO & info);

    DynLibHandle m_lib;
    MODULE_INFO m_moduleInfo;
    bool m_moduleInitialized = false;

private:
    ModuleBase(const ModuleBase &) = delete;
    ModuleBase & operator=(const ModuleBase &) = delete;

    void ModuleDone(bool callUnloadModule = true);
};