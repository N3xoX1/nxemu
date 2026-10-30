#pragma once
#include <memory>
#include <nxemu-module-spec/operating_system.h>

namespace Core
{
class System;
}

class EmuThread
{
public:
    EmuThread(Core::System & system_, Kernel::KProcess * & process);
    ~EmuThread();

    void Start();
    void StartPrepared(EmulationPrepareCallback prepare, void * userData);
    bool AwaitingRelease() const;
    void Release();
    void Stop();
    void SetRunning(bool should_run);
    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};