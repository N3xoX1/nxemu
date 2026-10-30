#pragma once
#include <memory>
#include <nxemu-module-spec/base.h>

nxinterface ISystemModules;
nxinterface IRenderWindow;

typedef bool (*LaunchPrepareCallback)(void * userData);

class SystemModules
{
public:
    SystemModules();
    ~SystemModules();

    void Setup(IRenderWindow & window);
    bool StartLaunch(IRenderWindow & window, LaunchPrepareCallback prepare, void * userData);
    bool IsLaunching() const;
    void CancelLaunch();
    bool LaunchDone() const;
    bool FinishLaunch();
    void ShutDown();
    void FlushSettings();
    bool IsValid() const;
    ISystemModules & Modules();

private:
    SystemModules(const SystemModules &) = delete;
    SystemModules & operator=(const SystemModules &) = delete;

    bool LoadModules();
    bool InitializeModules();
    static bool LaunchThread(void * userData);

    struct Impl;
    std::unique_ptr<Impl> impl;
};
