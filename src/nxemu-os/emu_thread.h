#pragma once
#include <memory>

namespace Core
{
class System;
}
namespace Kernel
{
class KProcess;
}

class EmuThread
{
public:
    EmuThread(Core::System & system_, Kernel::KProcess * & process);
    ~EmuThread();

    void Start();
    // Claim exclusive process ownership before the loader starts Run().
    // Returns false if Stop() already cancelled the launch.
    bool BeginProcessLaunch();
    // Release the startup gate once StartApplicationProcess has returned.
    // Failure and cancellation still unblock the guest control thread.
    void ProcessLaunchCompleted(bool succeeded);
    void Stop();
    void SetRunning(bool should_run);
    bool IsRunning() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
