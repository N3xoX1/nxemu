#pragma once
#include <memory>
#include <nxemu-module-spec/base.h>

nxinterface ISystemModules;
nxinterface IRenderWindow;

class SystemModules
{
public:
    SystemModules();
    ~SystemModules();

    void Setup(IRenderWindow & window);
    void ShutDown();
    void FlushSettings();
    bool IsValid() const;
    ISystemModules & Modules();

private:
    SystemModules(const SystemModules &) = delete;
    SystemModules & operator=(const SystemModules &) = delete;

    bool CreateModules();

    struct Impl;
    std::unique_ptr<Impl> impl;
};
