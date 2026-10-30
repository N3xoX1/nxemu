#include "system_modules.h"
#include "cpu_module.h"
#include "loader_module.h"
#include "module_notification.h"
#include "module_settings.h"
#include "operating_system_module.h"
#include "video_module.h"
#include "notification.h"
#include "settings/core_settings.h"
#include <atomic>
#include <chrono>
#include <future>
#include <vector>
#include <nxemu-core/settings/settings.h>
#include <nxemu-core/settings/identifiers.h>
#include <yuzu_common/logging/log.h>

namespace
{
typedef std::vector<ModuleBase *> BaseModules;

template <typename plugin_type>
void LoadModule(const std::string & fileName, std::unique_ptr<plugin_type> & plugin, ModuleNotification * moduleNotification, ModuleSettings * moduleSettings)
{
    Path fullPath((const char *)coreSettings.moduleDir, fileName.c_str());
    plugin = std::make_unique<plugin_type>();
    if (!fullPath.FileExists())
    {
        LOG_ERROR(Frontend, "Module setup: module not found ({})", (const char *)fullPath);
        plugin = nullptr;
    }
    else if (!plugin->Load(fullPath, moduleNotification, moduleSettings))
    {
        LOG_ERROR(Frontend, "Module setup: failed to load module ({})", (const char *)fullPath);
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
        SettingsStore & settings = SettingsStore::GetInstance();
        settings.SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Starting);

        for (BaseModules::iterator itr = baseModules.begin(); itr != baseModules.end(); itr++)
        {
            (*itr)->EmulationStarting();
        }
    }

    void StopEmulation(bool wait) override
    {
        if (stopping)
        {
            return;
        }
        stopping = true;

        SettingsStore & settings = SettingsStore::GetInstance();
        settings.SetInt(NXCoreSetting::EmulationState, (int32_t)EmulationState::Stopping);

        for (BaseModules::iterator itr = baseModules.begin(); itr != baseModules.end(); itr++)
        {
            (*itr)->EmulationStopping(wait);
        }
        if (settings.GetBool(NXCoreSetting::EmulationRunning))
        {
            settings.SetBool(NXCoreSetting::EmulationRunning, false);
        }

        stopping = false;
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
    bool stopping;
    bool launching = false;
    std::atomic<bool> launchCancelled{false};
    LaunchPrepareCallback launchPrepare = nullptr;
    void * launchUserData = nullptr;
    std::promise<bool> launchPromise;
    std::future<bool> launchResult;
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
    if (!LoadModules() || !InitializeModules())
    {
        ShutDown();
        return;
    }
    impl->operatingsystem->InitializeInput();
}

bool SystemModules::StartLaunch(IRenderWindow & window, LaunchPrepareCallback prepare, void * userData)
{
    ShutDown();
    impl = std::make_unique<Impl>(window);
    if (!LoadModules())
    {
        ShutDown();
        return false;
    }
    impl->launchPrepare = prepare;
    impl->launchUserData = userData;
    impl->launchResult = impl->launchPromise.get_future();
    impl->launching = true;
    impl->operatingsystem->StartEmulationThread(LaunchThread, this);
    return true;
}

bool SystemModules::IsLaunching() const
{
    return impl != nullptr && impl->launching;
}

void SystemModules::CancelLaunch()
{
    if (!IsLaunching())
    {
        return;
    }
    impl->launchCancelled.store(true, std::memory_order_relaxed);
    impl->systemLoader->CancelPrepareRom();
}

bool SystemModules::LaunchDone() const
{
    return IsLaunching() && impl->launchResult.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

bool SystemModules::FinishLaunch()
{
    if (!IsLaunching())
    {
        return false;
    }
    const bool prepared = impl->launchResult.get();
    impl->launching = false;
    return prepared;
}

bool SystemModules::LaunchThread(void * userData)
{
    SystemModules & modules = *static_cast<SystemModules *>(userData);
    Impl & impl = *modules.impl;
    const bool prepared = modules.InitializeModules() && !impl.launchCancelled.load(std::memory_order_relaxed) && impl.launchPrepare(impl.launchUserData);
    impl.launchPromise.set_value(prepared);
    return prepared;
}

bool SystemModules::LoadModules()
{
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
        return false;
    }
    LOG_INFO(Frontend, "Module setup: libraries loaded");

    impl->systemLoader = impl->loaderModule->CreateSystemLoader(*impl);
    if (impl->systemLoader == nullptr)
    {
        LOG_ERROR(Frontend, "Module setup: failed to create the loader ({})", impl->loaderFile);
        return false;
    }
    impl->cpu = impl->cpuModule->CreateCpu(*impl);
    if (impl->cpu == nullptr)
    {
        LOG_ERROR(Frontend, "Module setup: failed to create the cpu ({})", impl->cpuFile);
        return false;
    }
    impl->operatingsystem = impl->operatingsystemModule->CreateOS(*impl);
    if (impl->operatingsystem == nullptr)
    {
        LOG_ERROR(Frontend, "Module setup: failed to create the operating system ({})", impl->operatingsystemFile);
        return false;
    }
    impl->video = impl->videoModule->CreateVideo(impl->window, *impl);
    if (impl->video == nullptr)
    {
        LOG_ERROR(Frontend, "Module setup: failed to create the video ({})", impl->videoFile);
        return false;
    }
    LOG_INFO(Frontend, "Module setup: modules created");

    impl->baseModules.push_back(impl->cpuModule.get());
    impl->baseModules.push_back(impl->videoModule.get());
    impl->baseModules.push_back(impl->operatingsystemModule.get());
    impl->baseModules.push_back(impl->loaderModule.get());
    return true;
}

bool SystemModules::InitializeModules()
{
    if (!impl->systemLoader->Initialize())
    {
        LOG_ERROR(Frontend, "Module setup: failed to initialize the loader ({})", impl->loaderFile);
        return false;
    }
    LOG_INFO(Frontend, "Module setup: loader initialized");
    if (!impl->cpu->Initialize())
    {
        LOG_ERROR(Frontend, "Module setup: failed to initialize the cpu ({})", impl->cpuFile);
        return false;
    }
    LOG_INFO(Frontend, "Module setup: cpu initialized");
    if (!impl->video->Initialize())
    {
        LOG_ERROR(Frontend, "Module setup: failed to initialize the video ({})", impl->videoFile);
        return false;
    }
    LOG_INFO(Frontend, "Module setup: video initialized");
    if (!impl->operatingsystem->Initialize())
    {
        LOG_ERROR(Frontend, "Module setup: failed to initialize the operating system ({})", impl->operatingsystemFile);
        return false;
    }
    LOG_INFO(Frontend, "Module setup: operating system initialized");
    impl->valid = true;
    return true;
}

void SystemModules::ShutDown()
{
    if (impl == nullptr)
    {
        return;
    }
    if (impl->launching)
    {
        CancelLaunch();
        impl->launchResult.wait();
        impl->launching = false;
    }
    impl->StopEmulation(true);
    impl->baseModules.clear();
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
    // OS kernel objects hold interfaces created by the CPU module: release them first.
    if (impl->cpu != nullptr && impl->cpuModule.get() != nullptr)
    {
        impl->cpuModule->DestroyCpu(impl->cpu);
        impl->cpu = nullptr;
    }
    if (impl->systemLoader != nullptr && impl->loaderModule.get() != nullptr)
    {
        impl->loaderModule->DestroySystemLoader(impl->systemLoader);
        impl->systemLoader = nullptr;
    }
    impl = nullptr;
}

void SystemModules::FlushSettings(void)
{
    if (IsValid())
    {
        for (BaseModules::iterator itr = impl->baseModules.begin(); itr != impl->baseModules.end(); itr++)
        {
            (*itr)->FlushSettings();
        }
    }
    SaveCoreSetting();
}

bool SystemModules::IsValid() const
{
    return impl.get() != nullptr && !impl->launching && impl->IsValid();
}

ISystemModules & SystemModules::Modules()
{
    return *(impl.get());
}
