// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>

#include "yuzu_audio_core/audio_core.h"
#include "yuzu_common/microprofile.h"
#include "yuzu_common/hardware_properties.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/debugger/debugger.h"
#include "core/gpu_dirty_memory_manager.h"
#include "core/hle/kernel/k_memory_manager.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/apm/apm_controller.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/glue/glue_manager.h"
#include "core/hle/service/services.h"
#include "core/internal_network/network.h"
#include "core/perf_stats.h"
#include "core/reporter.h"
#include "yuzu_hid_core/hid_core.h"
#include "yuzu_input_common/main.h"
#include "network/network.h"
#include <nxemu-module-spec/system_loader.h>
#include <nxemu-video/video_settings_identifiers.h>

MICROPROFILE_DEFINE(ARM_CPU0, "ARM", "CPU 0", MP_RGB(255, 64, 64));
MICROPROFILE_DEFINE(ARM_CPU1, "ARM", "CPU 1", MP_RGB(255, 64, 64));
MICROPROFILE_DEFINE(ARM_CPU2, "ARM", "CPU 2", MP_RGB(255, 64, 64));
MICROPROFILE_DEFINE(ARM_CPU3, "ARM", "CPU 3", MP_RGB(255, 64, 64));

extern IModuleSettings * g_settings;

namespace Core {

struct System::Impl {
    explicit Impl(System & system_, ISystemModules & modules_) : 
        kernel{system_},
        hid_core{},
        cpu_manager{system_},
        room_network{},
        applet_manager{system_},
        frontend_applets{system_},
        modules(modules_),
        input_subsystem{std::make_shared<InputCommon::InputSubsystem>()},
        reporter{system_}
    {
        device_memory = std::make_unique<Core::DeviceMemory>();
    }

    ~Impl()
    {
        hid_core.UnloadInputDevices();
        input_subsystem->Shutdown();
    }

    void Initialize(System& system)
    {
        is_multicore = true; // Settings::values.use_multi_core.GetValue();

        core_timing.SetMulticore(is_multicore);
        core_timing.Initialize([&system]() { system.RegisterHostThread(); });

        // Create default implementations of applets if one is not provided.
        frontend_applets.SetDefaultAppletsIfMissing();

        is_async_gpu = g_settings->GetBool(NXVideoSetting::UseAsynchronousGPUEmulation);

        kernel.SetMulticore(is_multicore);
        cpu_manager.SetMulticore(is_multicore);
        cpu_manager.SetAsyncGpu(is_async_gpu);
        input_subsystem->Initialize();
    }

    void ReinitializeIfNecessary(System & system)
    {
        const bool must_reinitialize = false;
        //    is_multicore != Settings::values.use_multi_core.GetValue() ||
        //    extended_memory_layout != (Settings::values.memory_layout_mode.GetValue() !=
        //                               Settings::MemoryLayout::Memory_4Gb);

        if (!must_reinitialize)
        {
            return;
        }

        LOG_DEBUG(Kernel, "Re-initializing");

        // is_multicore = Settings::values.use_multi_core.GetValue();
        // extended_memory_layout =
        //     Settings::values.memory_layout_mode.GetValue() != Settings::MemoryLayout::Memory_4Gb;

        Initialize(system);
    }

    void Run()
    {
        std::unique_lock<std::mutex> lk(suspend_guard);

        kernel.SuspendEmulation(false);
        core_timing.SyncPause(false);
        is_paused.store(false, std::memory_order_relaxed);
    }

    void Pause()
    {
        std::unique_lock<std::mutex> lk(suspend_guard);

        core_timing.SyncPause(true);
        kernel.SuspendEmulation(true);
        is_paused.store(true, std::memory_order_relaxed);
    }

    bool IsPaused() const
    {
        return is_paused.load(std::memory_order_relaxed);
    }

    std::unique_lock<std::mutex> StallApplication()
    {
        std::unique_lock<std::mutex> lk(suspend_guard);
        kernel.SuspendEmulation(true);
        core_timing.SyncPause(true);
        return lk;
    }

    void UnstallApplication()
    {
        if (!IsPaused())
        {
            core_timing.SyncPause(false);
            kernel.SuspendEmulation(false);
        }
    }

    void SetNVDECActive(bool is_nvdec_active)
    {
        nvdec_active = is_nvdec_active;
    }

    bool GetNVDECActive()
    {
        return nvdec_active;
    }

    void InitializeKernel(System & system, uint64_t titleID)
    {
        LOG_DEBUG(Core, "initialized OK");

        // Setting changes may require a full system reinitialization (e.g., disabling multicore).
        ReinitializeIfNecessary(system);

        kernel.Initialize();
        cpu_manager.Initialize();
        arp_manager.ResetAll();
        audio_core = std::make_unique<AudioCore::AudioCore>(system);

        service_manager = std::make_shared<Service::SM::ServiceManager>(kernel);
        services =
            std::make_unique<Service::Services>(service_manager, system, stop_event.get_token());

        is_powered_on = true;
        exit_locked = false;
        exit_requested = false;

        microprofile_cpu[0] = MICROPROFILE_TOKEN(ARM_CPU0);
        microprofile_cpu[1] = MICROPROFILE_TOKEN(ARM_CPU1);
        microprofile_cpu[2] = MICROPROFILE_TOKEN(ARM_CPU2);
        microprofile_cpu[3] = MICROPROFILE_TOKEN(ARM_CPU3);

        perf_stats = std::make_unique<PerfStats>(titleID);

        // Reset counters and set time origin to current frame
        GetAndResetPerfStats();
        perf_stats->BeginSystemFrame();

        LOG_DEBUG(Core, "Initialized OK");
    }

    void ShutdownMainProcess() 
    {
        IVideo & video = modules.Video();

        SetShuttingDown(true);

        is_powered_on = false;
        exit_locked = false;
        exit_requested = false;

        stop_event.request_stop();
        core_timing.SyncPause(false);
        Network::CancelPendingSocketOperations();
        kernel.SuspendEmulation(true);
        kernel.CloseServices();
        kernel.ShutdownCores();
        applet_manager.Reset();
        services.reset();
        service_manager.reset();
        core_timing.ClearPendingEvents();
        audio_core.reset();
        perf_stats.reset();
        cpu_manager.Shutdown();
        kernel.Shutdown();
        stop_event = {};
        Network::RestartSocketOperations();

        if (auto room_member = room_network.GetRoomMember().lock())
        {
            Network::GameInfo game_info{};
            room_member->SendGameInfo(game_info);
        }

        LOG_DEBUG(Core, "Shutdown OK");
    }

    bool IsShuttingDown() const {
        return is_shutting_down;
    }

    void SetShuttingDown(bool shutting_down) {
        is_shutting_down = shutting_down;
    }

    PerfStatsResults GetAndResetPerfStats() {
        return perf_stats->GetAndResetStats(core_timing.GetGlobalTimeUs());
    }

    mutable std::mutex suspend_guard;
    std::atomic_bool is_paused{};
    std::atomic<bool> is_shutting_down{};

    Timing::CoreTiming core_timing;
    Kernel::KernelCore kernel;
    std::unique_ptr<Core::DeviceMemory> device_memory;
    std::unique_ptr<AudioCore::AudioCore> audio_core;
    Core::HID::HIDCore hid_core;
    Network::RoomNetwork room_network;
    std::shared_ptr<InputCommon::InputSubsystem> input_subsystem;

    CpuManager cpu_manager;
    std::atomic_bool is_powered_on{};
    bool exit_locked = false;
    bool exit_requested = false;

    bool nvdec_active{};

    Reporter reporter;
    std::array<u8, 0x20> build_id{};

    ISystemModules & modules;

    /// Applets
    Service::AM::AppletManager applet_manager;
    Service::AM::Frontend::FrontendAppletHolder frontend_applets;

    /// APM (Performance) services
    Service::APM::Controller apm_controller{core_timing};

    /// Service State
    Service::Glue::ARPManager arp_manager;

    /// Service manager
    std::shared_ptr<Service::SM::ServiceManager> service_manager;

    /// Services
    std::unique_ptr<Service::Services> services;

    std::unique_ptr<Core::PerfStats> perf_stats;
    Core::SpeedLimiter speed_limiter;

    bool is_multicore{};
    bool is_async_gpu{};

    ExecuteProgramCallback execute_program_callback;
    std::stop_source stop_event;

    std::array<u64, Hardware::NUM_CPU_CORES> dynarmic_ticks{};
    std::array<MicroProfileToken, Hardware::NUM_CPU_CORES> microprofile_cpu{};

    std::array<Core::GPUDirtyMemoryManager, Hardware::NUM_CPU_CORES>
        gpu_dirty_memory_managers;

    std::deque<std::vector<u8>> user_channel;
};

System::System(ISystemModules & modules) : 
    impl{std::make_unique<Impl>(*this, modules)} 
{
}

System::~System() = default;

CpuManager & System::GetCpuManager()
{
    return impl->cpu_manager;
}

const CpuManager & System::GetCpuManager() const
{
    return impl->cpu_manager;
}

void System::InitializeKernel(uint64_t titleID)
{
    impl->InitializeKernel(*this, titleID);
}

void System::Initialize() {
    impl->Initialize(*this);
}

void System::Run()
{
    impl->Run();
}

void System::Pause()
{
    impl->Pause();
}

bool System::IsPaused() const
{
    return impl->IsPaused();
}

bool System::IsShuttingDown() const
{
    return impl->IsShuttingDown();
}

void System::SetShuttingDown(bool shutting_down)
{
    impl->SetShuttingDown(shutting_down);
}

void System::ShutdownMainProcess()
{
    impl->ShutdownMainProcess();
}

std::unique_lock<std::mutex> System::StallApplication()
{
    return impl->StallApplication();
}

void System::UnstallApplication()
{
    impl->UnstallApplication();
}

void System::SetNVDECActive(bool is_nvdec_active)
{
    impl->SetNVDECActive(is_nvdec_active);
}

bool System::GetNVDECActive()
{
    return impl->GetNVDECActive();
}

bool System::IsPoweredOn() const
{
    return impl->is_powered_on.load(std::memory_order::relaxed);
}

size_t System::GetCurrentHostThreadID() const
{
    return impl->kernel.GetCurrentHostThreadID();
}

std::span<GPUDirtyMemoryManager> System::GetGPUDirtyMemoryManager()
{
    return impl->gpu_dirty_memory_managers;
}

void System::GatherGPUDirtyMemory(ICacheInvalidator * invalidator)
{
    for (auto & manager : impl->gpu_dirty_memory_managers)
    {
        manager.Gather(invalidator);
    }
}

PerfStatsResults System::GetAndResetPerfStats()
{
    return impl->GetAndResetPerfStats();
}

ISystemModules & System::GetSystemModules()
{
    return impl->modules;
}

IVideo & System::GetVideo()
{
    return impl->modules.Video();
}

ISystemloader & System::GetSystemloader()
{
    return impl->modules.Systemloader();
}

Kernel::PhysicalCore & System::CurrentPhysicalCore()
{
    return impl->kernel.CurrentPhysicalCore();
}

const Kernel::PhysicalCore & System::CurrentPhysicalCore() const
{
    return impl->kernel.CurrentPhysicalCore();
}

/// Gets the global scheduler
Kernel::GlobalSchedulerContext & System::GlobalSchedulerContext()
{
    return impl->kernel.GlobalSchedulerContext();
}

/// Gets the global scheduler
const Kernel::GlobalSchedulerContext & System::GlobalSchedulerContext() const
{
    return impl->kernel.GlobalSchedulerContext();
}

Kernel::KProcess * System::ApplicationProcess()
{
    return impl->kernel.ApplicationProcess();
}

Core::DeviceMemory & System::DeviceMemory()
{
    return *impl->device_memory;
}

const Core::DeviceMemory & System::DeviceMemory() const
{
    return *impl->device_memory;
}

Memory::Memory & System::ApplicationMemory()
{
    return impl->kernel.ApplicationProcess()->GetCoreMemory();
}

const Core::Memory::Memory & System::ApplicationMemory() const
{
    return impl->kernel.ApplicationProcess()->GetCoreMemory();
}

Kernel::KernelCore & System::Kernel()
{
    return impl->kernel;
}

const Kernel::KernelCore & System::Kernel() const
{
    return impl->kernel;
}

HID::HIDCore & System::HIDCore()
{
    return impl->hid_core;
}

const HID::HIDCore & System::HIDCore() const
{
    return impl->hid_core;
}

AudioCore::AudioCore & System::AudioCore()
{
    return *impl->audio_core;
}

const AudioCore::AudioCore & System::AudioCore() const
{
    return *impl->audio_core;
}

ICoreTiming & System::Timing()
{
    return impl->core_timing;
}

Timing::CoreTiming & System::CoreTiming()
{
    return impl->core_timing;
}

const Timing::CoreTiming & System::CoreTiming() const
{
    return impl->core_timing;
}

Core::PerfStats & System::GetPerfStats()
{
    return *impl->perf_stats;
}

const Core::PerfStats & System::GetPerfStats() const
{
    return *impl->perf_stats;
}

Core::SpeedLimiter & System::SpeedLimiter()
{
    return impl->speed_limiter;
}

const Core::SpeedLimiter & System::SpeedLimiter() const
{
    return impl->speed_limiter;
}

std::shared_ptr<InputCommon::InputSubsystem> & System::InputSubsystem() const
{
    return impl->input_subsystem;
}

u64 System::GetApplicationProcessProgramID() const {
    return impl->kernel.ApplicationProcess()->GetProgramId();
}

void System::AddGlueRegistrationForProcess(Kernel::KProcess & process, uint32_t version, StorageId baseGameStorageId, StorageId updateStorageId, uint8_t * nacpData, uint32_t nacpDataLen)
{
    std::vector<u8> nacp_data;
    if (nacpDataLen > 0)
    {
        nacp_data.resize(nacpDataLen);
        memcpy(nacp_data.data(), nacpData, nacpDataLen);
    }

    Service::Glue::ApplicationLaunchProperty launch{};
    launch.title_id = process.GetProgramId();
    launch.version = version;
    launch.base_game_storage_id = baseGameStorageId;
    launch.update_storage_id = updateStorageId;

    impl->arp_manager.Register(launch.title_id, launch, std::move(nacp_data));
}

void System::SetFrontendAppletSet(Service::AM::Frontend::FrontendAppletSet && set)
{
    impl->frontend_applets.SetFrontendAppletSet(std::move(set));
    impl->frontend_applets.SetDefaultAppletsIfMissing();
}

Service::AM::Frontend::FrontendAppletHolder & System::GetFrontendAppletHolder()
{
    return impl->frontend_applets;
}

const Service::AM::Frontend::FrontendAppletHolder & System::GetFrontendAppletHolder() const
{
    return impl->frontend_applets;
}

Service::AM::AppletManager & System::GetAppletManager()
{
    return impl->applet_manager;
}

const Reporter & System::GetReporter() const
{
    return impl->reporter;
}

Service::Glue::ARPManager & System::GetARPManager()
{
    return impl->arp_manager;
}

const Service::Glue::ARPManager & System::GetARPManager() const
{
    return impl->arp_manager;
}

Service::APM::Controller & System::GetAPMController()
{
    return impl->apm_controller;
}

const Service::APM::Controller & System::GetAPMController() const
{
    return impl->apm_controller;
}

void System::SetExitLocked(bool locked)
{
    impl->exit_locked = locked;
}

bool System::GetExitLocked() const
{
    return impl->exit_locked;
}

void System::SetExitRequested(bool requested)
{
    impl->exit_requested = requested;
}

bool System::GetExitRequested() const
{
    return impl->exit_requested;
}

void System::SetApplicationProcessBuildID(const CurrentBuildProcessID & id)
{
    impl->build_id = id;
}

const System::CurrentBuildProcessID & System::GetApplicationProcessBuildID() const
{
    return impl->build_id;
}

Service::SM::ServiceManager & System::ServiceManager()
{
    return *impl->service_manager;
}

const Service::SM::ServiceManager & System::ServiceManager() const
{
    return *impl->service_manager;
}

void System::RegisterCoreThread(std::size_t id)
{
    impl->kernel.RegisterCoreThread(id);
}

void System::RegisterHostThread()
{
    impl->kernel.RegisterHostThread();
}

void System::EnterCPUProfile()
{
    std::size_t core = impl->kernel.GetCurrentHostThreadID();
    impl->dynarmic_ticks[core] = MicroProfileEnter(impl->microprofile_cpu[core]);
}

void System::ExitCPUProfile()
{
    std::size_t core = impl->kernel.GetCurrentHostThreadID();
    MicroProfileLeave(impl->microprofile_cpu[core], impl->dynarmic_ticks[core]);
}

bool System::IsMulticore() const
{
    return impl->is_multicore;
}

bool System::DebuggerEnabled() const
{
    //return Settings::values.use_gdbstub.GetValue();
    return false;
}

Core::Debugger & System::GetDebugger()
{
    UNIMPLEMENTED();
    static Core::Debugger * ptr = nullptr;
    return *ptr;
}

const Core::Debugger & System::GetDebugger() const
{
    UNIMPLEMENTED();
    static Core::Debugger * ptr = nullptr;
    return *ptr;
}

Network::RoomNetwork & System::GetRoomNetwork()
{
    return impl->room_network;
}

const Network::RoomNetwork & System::GetRoomNetwork() const
{
    return impl->room_network;
}

void System::RunServer(std::unique_ptr<Service::ServerManager> && server_manager)
{
    return impl->kernel.RunServer(std::move(server_manager));
}

void System::RegisterExecuteProgramCallback(ExecuteProgramCallback && callback)
{
    impl->execute_program_callback = std::move(callback);
}

void System::ExecuteProgram(std::size_t program_index)
{
    if (impl->execute_program_callback)
    {
        impl->execute_program_callback(program_index);
    }
    else
    {
        LOG_CRITICAL(Core, "execute_program_callback must be initialized by the frontend");
    }
}

std::deque<std::vector<u8>> & System::GetUserChannel()
{
    return impl->user_channel;
}


void System::Exit()
{
    UNIMPLEMENTED();
}

} // namespace Core
