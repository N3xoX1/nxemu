#include "system_modules.h"
#include "cpu_module.h"
#include "loader_module.h"
#include "module_notification.h"
#include "module_settings.h"
#include "operating_system_module.h"
#include "video_module.h"
#include "notification.h"
#include "settings/core_settings.h"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <exception>
#include <yuzu_common/logging/log.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/settings/identifiers.h>

namespace
{
typedef std::vector<ModuleBase *> BaseModules;

template <typename plugin_type>
void LoadModule(const std::string & fileName, std::unique_ptr<plugin_type> & plugin, ModuleNotification * moduleNotification, ModuleSettings * moduleSettings)
{
    Path fullPath((const char *)coreSettings.moduleDir, fileName.c_str());
    plugin = std::make_unique<plugin_type>();
    if (plugin.get() == nullptr || !fullPath.FileExists() || !plugin->Load(fullPath, moduleNotification, moduleSettings))
    {
        plugin = nullptr;
    }
}
} // namespace

struct SystemModules::Impl :
    public ISystemModules
{
    explicit Impl(IRenderWindow & window_) :
        window(window_),
        systemLoader(nullptr),
        video(nullptr),
        cpu(nullptr),
        operatingsystem(nullptr),
        valid(false),
        frontendValid(false),
        stopping(false)
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        settings.RegisterCallback(NXCoreSetting::EmulationRunning, EmulationRunningChanged, this);
    }

    ~Impl()
    {
        SettingsStore & settings = SettingsStore::GetInstance();
        settings.UnregisterCallback(NXCoreSetting::EmulationRunning, EmulationRunningChanged, this);    
    }
    
    static void EmulationRunningChanged(const char * /*setting*/, void * userData)
    {
        SystemModules::Impl & this_ = *((SystemModules::Impl *)userData);
        SettingsStore & settings = SettingsStore::GetInstance();
        std::lock_guard<std::recursive_mutex> lock(this_.startStopMutex);
        if (!this_.valid)
        {
            return;
        }
        // Read only after acquiring the transition lock. A notification may
        // have waited behind a newer stop request from another thread.
        if (settings.GetBool(NXCoreSetting::EmulationRunning))
        {
            this_.StartEmulation();
        }
        else
        {
            this_.StopEmulation(false);
        }
    }

    bool IsValid() const override
    {
        return valid;
    }

    void StartEmulation() override
    {
        std::lock_guard<std::recursive_mutex> lock(startStopMutex);
        if (!valid || stopping)
        {
            return;
        }
        SettingsStore & settings = SettingsStore::GetInstance();
        settings.SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Starting);

        for (ModuleBase * module : baseModules)
        {
            module->EmulationStarting();
            if (module == videoModule.get() && !video->GuestGpuCreated())
            {
                // The OS must never start without a valid guest GPU renderer.
                // No OS control thread exists yet, so explicitly finalize the
                // stopped state after the regular module cleanup callbacks.
                StopEmulation(false);
                settings.SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopped);
                return;
            }
        }
    }

    void StopEmulation(bool wait) override
    {
        std::unique_lock<std::recursive_mutex> lock(startStopMutex);
        if (stopping)
        {
            // The OS thread can signal StopEmulation(false) while the UI
            // thread is joining it. That notification must never wait for its
            // own join. A second *blocking* caller, however, must not return
            // before the first stop has completed.
            if (!wait || stoppingThread == std::this_thread::get_id())
            {
                return;
            }
            const bool alreadyJoined = stoppingWait;
            stopCompleted.wait(lock, [this] { return !stopping; });
            if (alreadyJoined && stopSucceeded)
            {
                return;
            }
            // The first stop was non-blocking: perform a real join below.
        }
        stopping = true;
        stoppingWait = wait;
        stopSucceeded = false;
        stoppingThread = std::this_thread::get_id();

        try
        {
            SettingsStore & settings = SettingsStore::GetInstance();
            settings.SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopping);

            for (ModuleBase * module : baseModules)
            {
                // EmulationStopping(true) may join the guest control thread.
                // Never hold the transition mutex while joining it: that thread
                // can be completing a failed startup and sending a stop event.
                if (wait)
                {
                    lock.unlock();
                }
                try
                {
                    module->EmulationStopping(wait);
                }
                catch (...)
                {
                    if (wait)
                    {
                        lock.lock();
                    }
                    throw;
                }
                if (wait)
                {
                    lock.lock();
                }
            }
            if (settings.GetBool(NXCoreSetting::EmulationRunning))
            {
                settings.SetBool(NXCoreSetting::EmulationRunning, false);
            }
        }
        catch (...)
        {
            stopping = false;
            stoppingThread = {};
            lock.unlock();
            stopCompleted.notify_all();
            throw;
        }
        stopping = false;
        stopSucceeded = true;
        stoppingThread = {};
        lock.unlock();
        stopCompleted.notify_all();
    }

    ISystemloader & Systemloader() override
    {
        return *systemLoader;
    }

    IOperatingSystem & OperatingSystem() override
    {
        return *operatingsystem;
    }

    IVideo & Video() override
    {
        return *video;
    }

    ICpu & Cpu() override
    {
        return *cpu;
    }

    IRenderWindow & window;
    BaseModules baseModules;
    ModuleNotification moduleNotification;
    ModuleSettings moduleSettings;
    std::string loaderFile;
    std::string cpuFile;
    std::string videoFile;
    std::string operatingsystemFile;
    std::unique_ptr<LoaderModule> loaderModule;
    std::unique_ptr<CpuModule> cpuModule;
    std::unique_ptr<VideoModule> videoModule;
    std::unique_ptr<OperatingSystemModule> operatingsystemModule;
    ISystemloader * systemLoader;
    IVideo * video;
    ICpu * cpu;
    IOperatingSystem * operatingsystem;
    std::recursive_mutex startStopMutex; // Session transitions only; never used per frame.
    std::condition_variable_any stopCompleted;
    std::thread::id stoppingThread;
    bool stoppingWait = false;
    bool stopSucceeded = false;
    bool valid;
    bool frontendValid;
    bool stopping;
};

SystemModules::SystemModules()
{
}

SystemModules::~SystemModules()
{
    ShutDown();
}

void SystemModules::Setup(IRenderWindow & window)
{
    // Preserve the eager contract for the legacy frontend.
    if (SetupFrontend(window) && !StartSession())
    {
        ShutDown();
    }
}

bool SystemModules::SetupFrontend(IRenderWindow & window)
{
    if (!SetupDisplay(window))
        return false;
    if (CompleteFrontendSetup())
        return true;
    ShutDown();
    return false;
}

bool SystemModules::SetupDisplay(IRenderWindow & window)
{
    ShutDown();
    impl = std::make_unique<Impl>(window);
    impl->loaderFile = coreSettings.moduleLoader;
    impl->videoFile = coreSettings.moduleVideo;
    impl->cpuFile = coreSettings.moduleCpu;
    impl->operatingsystemFile = coreSettings.moduleOs;

    LoadModule(impl->videoFile, impl->videoModule, &impl->moduleNotification, &impl->moduleSettings);
    if (!impl->videoModule)
    {
        ShutDown();
        return false;
    }
    impl->video = impl->videoModule->CreateVideo(impl->window, *impl);
    if (!impl->video || !impl->video->Initialize())
    {
        ShutDown();
        return false;
    }
    return true;
}

bool SystemModules::CompleteFrontendSetup()
{
    if (!DisplayReady())
        return false;
    if (impl->frontendValid)
        return true;
    // On failure keep the display alive until the caller detaches its UI.
    // SetupFrontend() supplies the eager all-or-nothing convenience contract.
    LoadModule(impl->loaderFile, impl->loaderModule, &impl->moduleNotification, &impl->moduleSettings);
    // Both plugins register settings required by the idle frontend. Keep the
    // DLLs loaded, but construct neither the CPU/JIT nor the guest OS here.
    LoadModule(impl->cpuFile, impl->cpuModule, &impl->moduleNotification, &impl->moduleSettings);
    LoadModule(impl->operatingsystemFile, impl->operatingsystemModule, &impl->moduleNotification, &impl->moduleSettings);
    if (!impl->loaderModule || !impl->videoModule || !impl->cpuModule || !impl->operatingsystemModule)
    {
        return false;
    }
    impl->systemLoader = impl->loaderModule->CreateSystemLoader(*impl);
    if (!impl->systemLoader || !impl->systemLoader->Initialize())
    {
        return false;
    }
    impl->baseModules = {impl->cpuModule.get(), impl->videoModule.get(), impl->operatingsystemModule.get(), impl->loaderModule.get()};
    impl->frontendValid = true;
    return true;
}

bool SystemModules::DisplayReady() const
{
    return impl && impl->video;
}

bool SystemModules::StartSession()
{
    if (!FrontendValid())
    {
        return false;
    }
    if (impl->valid)
    {
        return true;
    }

    if (!impl->cpuModule || !impl->operatingsystemModule)
    {
        StopSession();
        return false;
    }
    try
    {
        impl->cpu = impl->cpuModule->CreateCpu(*impl);
        impl->operatingsystem = impl->operatingsystemModule->CreateOS(*impl);
        if (impl->cpu && impl->operatingsystem && impl->cpu->Initialize() &&
            impl->video->PrepareGuest() && impl->operatingsystem->Initialize())
        {
            impl->valid = true;
            return true;
        }
    }
    catch (const std::exception & error)
    {
        LOG_ERROR(Core, "Guest session initialization failed: {}", error.what());
    }
    catch (...)
    {
        LOG_ERROR(Core, "Guest session initialization failed with an unknown exception");
    }
    StopSession();
    return false;
}

void SystemModules::StopSession()
{
    if (!impl)
    {
        return;
    }
    bool was_valid = false;
    {
        // Invalidate the guest while holding the transition lock, but never
        // keep the lock across an OS-thread join in StopEmulation(true).
        std::lock_guard<std::recursive_mutex> lock(impl->startStopMutex);
        was_valid = impl->valid;
        impl->valid = false;
    }
    if (was_valid)
    {
        impl->StopEmulation(true);
        // The OS plugin remains loaded for its frontend settings schema.
        for (ModuleBase * module : impl->baseModules)
        {
            module->FlushSettings();
        }
    }
    impl->baseModules.clear();

    // The guest GPU must no longer refer to OS memory before the OS dies.
    if (impl->video)
    {
        impl->video->ReleaseGuest();
    }
    // Match the legacy core teardown order after the guest GPU is quiescent.
    if (impl->cpu && impl->cpuModule)
    {
        impl->cpuModule->DestroyCpu(impl->cpu);
        impl->cpu = nullptr;
    }
    if (impl->operatingsystem && impl->operatingsystemModule)
    {
        impl->operatingsystemModule->DestroyOS(impl->operatingsystem);
        impl->operatingsystem = nullptr;
    }
    // The OS no longer borrows content-provider entries or loaded files.
    if (impl->systemLoader)
    {
        impl->systemLoader->ResetSession();
    }
    // Preserve the CPU settings callbacks and saved configuration even when
    // its execution engine is absent between sessions.
    if (impl->frontendValid)
    {
        impl->baseModules = {impl->cpuModule.get(), impl->videoModule.get(), impl->operatingsystemModule.get(), impl->loaderModule.get()};
    }
}

void SystemModules::ShutDown()
{
    if (!impl)
    {
        return;
    }
    StopSession();
    impl->baseModules.clear();
    impl->frontendValid = false;
    if (impl->video && impl->videoModule)
    {
        impl->videoModule->DestroyVideo(impl->video);
        impl->video = nullptr;
    }
    if (impl->systemLoader && impl->loaderModule)
    {
        impl->loaderModule->DestroySystemLoader(impl->systemLoader);
        impl->systemLoader = nullptr;
    }
    impl.reset();
}

void SystemModules::FlushSettings()
{
    if (!impl)
    {
        return;
    }
    for (ModuleBase * module : impl->baseModules)
    {
        module->FlushSettings();
    }
}

bool SystemModules::IsValid() const
{
    return impl && impl->valid;
}

bool SystemModules::FrontendValid() const
{
    return impl && impl->frontendValid;
}

ISystemloader & SystemModules::Loader()
{
    return *impl->systemLoader;
}

IVideo & SystemModules::DisplayVideo()
{
    return *impl->video;
}

ISystemModules & SystemModules::Modules()
{
    return *impl;
}
