#pragma once
#include <memory>
#include <nxemu-module-spec/base.h>

nxinterface ISystemModules;
nxinterface IRenderWindow;
nxinterface ISystemloader;
nxinterface IVideo;

class SystemModules
{
public:
    SystemModules();
    ~SystemModules();

    // Legacy eager setup for non-RmlUI frontends.
    void Setup(IRenderWindow & window);
    // The RmlUI host needs only loader and display before a game is launched.
    bool SetupFrontend(IRenderWindow & window);
    bool StartSession();
    void StopSession();
    void ShutDown();
    void FlushSettings();
    bool IsValid() const; // Guest modules are present.
    bool FrontendValid() const;
    ISystemloader & Loader();
    IVideo & DisplayVideo();
    ISystemModules & Modules();

private:
    SystemModules(const SystemModules &) = delete;
    SystemModules & operator=(const SystemModules &) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl;
};
