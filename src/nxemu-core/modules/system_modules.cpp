#include "system_modules.h"
#include "cpu_module.h"
#include "loader_module.h"
#include "module_notification.h"
#include "module_settings.h"
#include "operating_system_module.h"
#include "video_module.h"
#include "notification.h"
#include "settings/core_settings.h"
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/settings/identifiers.h>
#include <yuzu_common/scope_exit.h>

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
    enum class LifecycleState { Idle, Starting, Stopping };

    explicit Impl(IRenderWindow & window_) :
        window(window_),
        systemLoader(nullptr),
        video(nullptr),
        cpu(nullptr),
        operatingsystem(nullptr),
        valid(false)
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
        bool emulationRunning = settings.GetBool(NXCoreSetting::EmulationRunning);
        if (emulationRunning)
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
        if (!BeginLifecycle(LifecycleState::Starting, true))
            return;
        auto transition = SCOPE_GUARD { EndLifecycle(); };
        try
        {
            if (pendingAsyncStop)
            {
                // Complete the previous session before initializing a new renderer.
                {
                    std::scoped_lock lock{lifecycleMutex};
                    lifecycleState = LifecycleState::Stopping;
                }
                if (operatingsystemModule)
                    operatingsystemModule->EmulationStopping(true);
                if (videoModule)
                    videoModule->EmulationStopping(true);
                pendingAsyncStop = false;
                {
                    std::scoped_lock lock{lifecycleMutex};
                    // Errors from the old GPU are covered by its completed stop.
                    asyncStopRequested = false;
                    lifecycleState = LifecycleState::Starting;
                }
            }
            SettingsStore & settings = SettingsStore::GetInstance();
            settings.SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Starting);
            for (BaseModules::iterator itr = baseModules.begin(); itr != baseModules.end(); itr++)
            {
                (*itr)->EmulationStarting();
                // A module that cannot start requests a stop. Do not start the
                // following modules, in particular the guest after a video failure.
                std::scoped_lock lock{lifecycleMutex};
                if (asyncStopRequested)
                    break;
            }
        }
        catch (const std::exception & error)
        {
            // Modules report their own failures with a stop request. This only
            // covers errors raised while the core itself drives the start.
            {
                std::scoped_lock lock{lifecycleMutex};
                lifecycleState = LifecycleState::Stopping;
            }
            StopModules(true);
            moduleNotification.DisplayError(error.what(), "Emulation initialization failed");
            return;
        }
        {
            std::scoped_lock lock{lifecycleMutex};
            if (!asyncStopRequested)
            {
                // Publish completion atomically with the pending-request check.
                transition.Cancel();
                EndLifecycleLocked();
                return;
            }
            lifecycleState = LifecycleState::Stopping;
        }
        // A GPU error during startup must not wait for startup to release a lock.
        StopModules(false);
    }

    void StopEmulation(bool wait) override
    {
        if (!BeginLifecycle(LifecycleState::Stopping, wait))
            return;
        SCOPE_EXIT { EndLifecycle(); };
        StopModules(wait);
    }

    bool BeginLifecycle(LifecycleState next, bool wait)
    {
        std::unique_lock lock{lifecycleMutex};
        if (shuttingDown)
            return false;
        if (!wait && lifecycleState != LifecycleState::Idle)
        {
            // GPU stop requests cannot block a thread that is joining the GPU.
            // An active stop already covers them; an active start defers them.
            if (lifecycleState == LifecycleState::Starting)
                asyncStopRequested = true;
            return false;
        }
        if (lifecycleState != LifecycleState::Idle &&
            lifecycleThread == std::this_thread::get_id())
            return false;
        ++lifecycleWaiters;
        auto waiter = SCOPE_GUARD {
            --lifecycleWaiters;
            lifecycleCv.notify_all();
        };
        lifecycleCv.wait(lock, [this] {
            return lifecycleState == LifecycleState::Idle || shuttingDown;
        });
        if (shuttingDown)
            return false;
        lifecycleState = next;
        lifecycleThread = std::this_thread::get_id();
        return true;
    }

    bool BeginShutdown()
    {
        std::unique_lock lock{lifecycleMutex};
        if (lifecycleState != LifecycleState::Idle &&
            lifecycleThread == std::this_thread::get_id())
            return false;
        // Close the gate before waiting, and keep it closed during destruction.
        shuttingDown = true;
        lifecycleCv.notify_all();
        lifecycleCv.wait(lock, [this] {
            return lifecycleState == LifecycleState::Idle && lifecycleWaiters == 0;
        });
        lifecycleState = LifecycleState::Stopping;
        lifecycleThread = std::this_thread::get_id();
        return true;
    }

    void EndLifecycleLocked()
    {
        asyncStopRequested = false;
        lifecycleState = LifecycleState::Idle;
        lifecycleThread = {};
        lifecycleCv.notify_all();
    }

    void EndLifecycle()
    {
        std::scoped_lock lock{lifecycleMutex};
        EndLifecycleLocked();
    }

    void StopModules(bool wait)
    {
        // Lifecycle ownership serializes module calls. Its mutex is released
        // throughout callbacks and joins so GPU error requests can return.
        pendingAsyncStop = true;
        SettingsStore & settings = SettingsStore::GetInstance();
        settings.SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopping);

        // A synchronous shutdown must join the emulation/producer thread
        // before releasing GPU resources in the video module.
        if (wait && operatingsystemModule)
            operatingsystemModule->EmulationStopping(true);
        for (BaseModules::iterator itr = baseModules.begin(); itr != baseModules.end(); itr++)
        {
            if (wait && *itr == operatingsystemModule.get())
                continue;
            (*itr)->EmulationStopping(wait);
        }
        if (settings.GetBool(NXCoreSetting::EmulationRunning))
        {
            settings.SetBool(NXCoreSetting::EmulationRunning, false);
        }

        pendingAsyncStop = !wait;
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
    bool valid;
    std::mutex lifecycleMutex;
    std::condition_variable lifecycleCv;
    LifecycleState lifecycleState = LifecycleState::Idle;
    std::thread::id lifecycleThread;
    std::size_t lifecycleWaiters = 0;
    bool asyncStopRequested = false;
    bool shuttingDown = false;
    bool pendingAsyncStop = false;
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
    ShutDown();
    impl = std::make_unique<Impl>(window);
    impl->valid = false;

    impl->loaderFile = coreSettings.moduleLoader;
    impl->cpuFile = coreSettings.moduleCpu;
    impl->videoFile = coreSettings.moduleVideo;
    impl->operatingsystemFile = coreSettings.moduleOs;

    LoadModule(impl->loaderFile, impl->loaderModule, &impl->moduleNotification, &impl->moduleSettings);
    LoadModule(impl->cpuFile, impl->cpuModule, &impl->moduleNotification, &impl->moduleSettings);
    LoadModule(impl->videoFile, impl->videoModule, &impl->moduleNotification, &impl->moduleSettings);
    LoadModule(impl->operatingsystemFile, impl->operatingsystemModule, &impl->moduleNotification, &impl->moduleSettings);

    if (impl->loaderModule.get() == nullptr || 
        impl->cpuModule.get() == nullptr || 
        impl->videoModule.get() == nullptr || 
        impl->operatingsystemModule.get() == nullptr)
    {
        return;
    }
    impl->systemLoader = impl->loaderModule->CreateSystemLoader(*impl);
    if (impl->systemLoader == nullptr)
    {
        return;
    }
    impl->cpu = impl->cpuModule->CreateCpu(*impl);
    if (impl->cpu == nullptr)
    {
        return;
    }
    impl->operatingsystem = impl->operatingsystemModule->CreateOS(*impl);
    if (impl->operatingsystem == nullptr)
    {
        return;
    }
    impl->video = impl->videoModule->CreateVideo(impl->window, *impl);
    if (impl->video == nullptr)
    {
        return;
    }

    if (!impl->systemLoader->Initialize())
    {
        return;
    }
    if (!impl->cpu->Initialize())
    {
        return;
    }
    if (!impl->video->Initialize())
    {
        return;
    }
    if (!impl->operatingsystem->Initialize())
    {
        return;
    }

    impl->baseModules.push_back(impl->cpuModule.get());
    impl->baseModules.push_back(impl->videoModule.get());
    impl->baseModules.push_back(impl->operatingsystemModule.get());
    impl->baseModules.push_back(impl->loaderModule.get());
    impl->valid = true;
}

void SystemModules::ShutDown()
{
    if (impl == nullptr)
    {
        return;
    }
    if (!impl->BeginShutdown())
        return;
    // Keep lifecycle ownership through module destruction, not only StopModules.
    auto transition = SCOPE_GUARD { impl->EndLifecycle(); };
    impl->StopModules(true);
    if (impl->cpu != nullptr && impl->cpuModule.get() != nullptr)
    {
        impl->cpuModule->DestroyCpu(impl->cpu);
        impl->cpu = nullptr;
    }
    if (impl->video != nullptr && impl->videoModule.get() != nullptr)
    {
        impl->videoModule->DestroyVideo(impl->video);
        impl->video = nullptr;
    }
    if (impl->operatingsystem != nullptr && impl->operatingsystemModule.get() != nullptr)
    {
        impl->operatingsystemModule->DestroyOS(impl->operatingsystem);
        impl->operatingsystem = nullptr;
    }
    if (impl->systemLoader != nullptr && impl->loaderModule.get() != nullptr)
    {
        impl->loaderModule->DestroySystemLoader(impl->systemLoader);
        impl->systemLoader = nullptr;
    }
    for (BaseModules::iterator itr = impl->baseModules.begin(); itr != impl->baseModules.end(); itr++)
    {
        (*itr)->ModuleCleanup();
    }
    impl->baseModules.clear();
    transition.Cancel();
    impl = nullptr;
}

void SystemModules::FlushSettings(void)
{
    for (BaseModules::iterator itr = impl->baseModules.begin(); itr != impl->baseModules.end(); itr++)
    {
        (*itr)->FlushSettings();
    }
}

bool SystemModules::IsValid() const
{
    return impl.get() != nullptr ? impl->IsValid() : false;
}

ISystemModules & SystemModules::Modules()
{
    return *(impl.get());
}
