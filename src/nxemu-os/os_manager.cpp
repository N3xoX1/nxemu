#include "os_manager.h"
#include "profile_image_writer.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_scheduler.h"
#include "yuzu_common/logging/log.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/perf_stats.h"
#include "os_settings.h"
#include "os_settings_identifiers.h"
#include "yuzu_audio_core/sink/sink_details.h"
#include "yuzu_common/fs/path_util.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/string_util.h"
#include "yuzu_hid_core/frontend/emulated_controller.h"
#include "yuzu_hid_core/hid_core.h"
#include "yuzu_input_common/drivers/keyboard.h"
#include "yuzu_input_common/drivers/virtual_gamepad.h"
#include "yuzu_input_common/main.h"
#include <nxemu-core/settings/identifiers.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fmt/ranges.h>
#include <string>

#if NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
namespace Kernel
{
// Capture only on explicit request after a hang. Never read a running JIT's registers.
class KernelStateSnapshot
{
    struct ThreadRow
    {
        u64 id{}, owner{}, affinity{}, last_scheduled{}, argument{}, tls{};
        u64 lock_owner{}, address{}, condvar{}, svc_count{}, svc_arg0{}, svc_arg1{};
        s64 deadline{};
        s32 priority{}, base_priority{}, active_core{}, wait_reason{};
        u32 state{}, wait_result{}, svc_id{};
        bool context_available{}, in_svc{};
        CpuThreadContext context{};
        std::vector<KThread::DiagnosticEvent> events;
        u64 event_count{};
    };
    struct CoreRow
    {
        u64 current{}, highest{}, pinned{}, virtual_ticks{}, virtual_deadline{};
        bool needs_scheduling{};
        std::vector<u64> scheduled, suggested;
    };
    static u64 Id(const KThread* thread) { return thread ? thread->GetThreadId() : 0; }

public:
    static void Dump(KernelCore& kernel, KProcess& process)
    {
        std::vector<ThreadRow> threads;
        std::array<CoreRow, Hardware::NUM_CPU_CORES> cores;
        s64 now{}, wakeup{}, next_task{};
        bool update_needed{};
        {
            // Match the process-list -> scheduler -> timer lock order used by the kernel.
            // List ownership prevents a thread from being destroyed during the snapshot.
            KScopedLightLock list_lock{process.GetListLock()};
            KScopedSchedulerLock scheduler_lock{kernel};
            auto& global = kernel.GlobalSchedulerContext();
            auto& timer = kernel.HardwareTimer();
            KScopedSpinLock timer_lock{timer.GetLock()};
            now = timer.GetTick();
            wakeup = timer.m_wakeup_time;
            next_task = timer.m_next_task ? timer.m_next_task->GetTime() : 0;
            update_needed = global.m_scheduler_update_needed.load();
            for (s32 core = 0; core < static_cast<s32>(cores.size()); ++core)
            {
                auto& scheduler = kernel.Scheduler(core);
                auto& row = cores[core];
                row.current = Id(scheduler.GetSchedulerCurrentThread());
                row.highest = Id(scheduler.m_state.highest_priority_thread.load(std::memory_order_acquire));
                row.pinned = Id(process.GetPinnedThread(core));
                row.needs_scheduling = scheduler.m_state.needs_scheduling.load();
                row.virtual_ticks = kernel.System().CoreTiming().GetGlobalTimeNs().count();
                row.virtual_deadline = 0;
                auto& queue = global.m_priority_queue;
                for (auto* thread = queue.GetScheduledFront(core); thread;
                     thread = queue.GetScheduledNext(core, thread))
                    row.scheduled.push_back(Id(thread));
                for (auto* thread = queue.GetSuggestedFront(core); thread;
                     thread = queue.GetSuggestedNext(core, thread))
                    row.suggested.push_back(Id(thread));
            }
            for (auto& thread : process.GetThreadList())
            {
                ThreadRow row;
                row.id = thread.GetThreadId();
                row.owner = process.GetProcessId();
                row.state = static_cast<u32>(thread.GetRawState());
                row.priority = thread.GetPriority();
                row.base_priority = thread.GetBasePriority();
                row.active_core = thread.GetActiveCore();
                row.affinity = thread.GetAffinityMask().GetAffinityMask();
                row.last_scheduled = thread.GetLastScheduledTick();
                row.argument = thread.GetArgument();
                row.tls = thread.GetTlsAddress().GetValue();
                row.svc_count = thread.diagnostic_svc_count.load(std::memory_order_relaxed);
                row.svc_id = thread.diagnostic_svc_id.load(std::memory_order_relaxed);
                row.svc_arg0 = thread.diagnostic_svc_arg0.load(std::memory_order_relaxed);
                row.svc_arg1 = thread.diagnostic_svc_arg1.load(std::memory_order_relaxed);
                row.in_svc = thread.diagnostic_in_svc.load(std::memory_order_relaxed);
                {
                    std::scoped_lock history_lock{thread.diagnostic_event_guard};
                    row.event_count = thread.diagnostic_event_count;
                    const auto count = std::min<u64>(row.event_count, thread.diagnostic_events.size());
                    for (u64 i = row.event_count - count; i < row.event_count; ++i)
                        row.events.push_back(thread.diagnostic_events[i % thread.diagnostic_events.size()]);
                }
                if (thread.GetState() == ThreadState::Waiting)
                {
                    row.wait_reason = static_cast<s32>(thread.GetWaitReasonForDebugging());
                    row.wait_result = thread.GetWaitResult().raw;
                    row.lock_owner = Id(thread.GetLockOwner());
                    row.address = thread.GetAddressKey().GetValue();
                    row.condvar = thread.GetConditionVariableKey();
                    row.deadline = thread.GetTime();
                }
                // Never wait for a context: its owner may need the scheduler lock we hold.
                std::unique_lock context_lock{thread.m_context_guard, std::try_to_lock};
                if (context_lock.owns_lock())
                {
                    row.context = thread.GetContext();
                    row.context_available = true;
                }
                threads.push_back(row);
            }
        }
        // Formatting and log I/O happen after releasing all kernel locks.
        LOG_INFO(Kernel, "KernelSnapshot BEGIN program={:016X} process={} multicore={} now_ns={} timer_wakeup_ns={} next_task_ns={} update_needed={}",
                 process.GetProgramId(), process.GetProcessId(), kernel.IsMulticore(), now, wakeup, next_task, update_needed);
        for (size_t core = 0; core < cores.size(); ++core)
        {
            const auto& row = cores[core];
            LOG_INFO(Kernel, "KernelSnapshot core={} current={} highest={} pinned={} needs_scheduling={} scheduled=[{}] suggested=[{}]",
                     core, row.current, row.highest, row.pinned, row.needs_scheduling,
                     fmt::join(row.scheduled, ","), fmt::join(row.suggested, ","));
            LOG_INFO(Kernel, "KernelSnapshot core={} virtual_ticks={} virtual_ipc_deadline={}",
                     core, row.virtual_ticks, row.virtual_deadline);
        }
        for (const auto& row : threads)
        {
            LOG_INFO(Kernel, "KernelSnapshot thread={} process={} state={:#x} priority={} base_priority={} active_core={} affinity={:#x} arg={:#x} tls={:#x} last_scheduled_cntpct={} wait_reason={} lock_owner={} address={:#x} condvar={:#x} deadline_ns={} wait_result={:#x}",
                     row.id, row.owner, row.state, row.priority, row.base_priority, row.active_core,
                     row.affinity, row.argument, row.tls, row.last_scheduled, row.wait_reason,
                     row.lock_owner, row.address, row.condvar, row.deadline, row.wait_result);
            // Independent atomic samples: arguments may straddle two calls on a running thread.
            LOG_INFO(Kernel, "KernelSnapshot thread={} sampled_svc_count={} sampled_svc={:#x} sampled_x0={:#x} sampled_x1={:#x} sampled_in_svc={}",
                     row.id, row.svc_count, row.svc_id, row.svc_arg0, row.svc_arg1, row.in_svc);
            if (row.context_available)
            {
                LOG_INFO(Kernel, "KernelSnapshot thread={} saved_pc={:#x} saved_lr={:#x} saved_sp={:#x} saved_x0={:#x} saved_x1={:#x} saved_x19={:#x} saved_x20={:#x}",
                         row.id, row.context.pc, row.context.lr, row.context.sp, row.context.r[0],
                         row.context.r[1], row.context.r[19], row.context.r[20]);
                LOG_INFO(Kernel, "KernelSnapshot thread={} saved_x21={:#x} saved_x22={:#x} saved_x23={:#x} saved_x24={:#x} saved_x25={:#x} saved_x26={:#x} saved_x27={:#x} saved_x28={:#x}",
                         row.id, row.context.r[21], row.context.r[22], row.context.r[23], row.context.r[24],
                         row.context.r[25], row.context.r[26], row.context.r[27], row.context.r[28]);
            }
            else
            {
                LOG_INFO(Kernel, "KernelSnapshot thread={} saved_context=unavailable (context owned by a core)", row.id);
            }
            LOG_INFO(Kernel, "KernelHistory thread={} events_total={} events_retained={} kinds=0:run,1:svc_enter,2:svc_return,3:ipc_wait,4:ipc_reply,7:hle_enter,8:hle_return; times=host_steady_ns", row.id, row.event_count, row.events.size());
            for (const auto& event : row.events)
            {
                const auto& v = event.values;
                LOG_INFO(Kernel, "KernelHistory thread={} time_ns={} kind={} v0={:#x} v1={:#x} v2={:#x} v3={:#x} v4={:#x} v5={:#x} v6={:#x} v7={:#x} jit_ns={} translate_ns={} optimize_ns={} emit_ns={} blocks={} interrupt_ns={} halt_lookup={:#x} halt_entry={:#x} guest_entered={}",
                    row.id, event.time_ns, event.kind, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                    event.jit.compile_ns, event.jit.translate_ns, event.jit.optimize_ns, event.jit.emit_ns,
                    event.jit.compiled_blocks, event.jit.interrupt_ns, event.jit.halt_before_lookup, event.jit.halt_before_entry, event.jit.guest_entered);
            }
        }
        LOG_INFO(Kernel, "KernelSnapshot END threads={} state=0:init,1:waiting,2:runnable,3:terminated; suspend_bits=0x3f0; wait_reason=0:none,1:sleep,2:ipc,3:sync,4:condvar,5:arbiter,6:suspended", threads.size());
    }
};
}
#endif

void OSManager::DumpKernelState()
{
#if NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
    if (!IsPoweredOn() || IsShuttingDown() || !m_applicationProcess) return;
    RegisterHostThread();
    Kernel::KernelStateSnapshot::Dump(m_coreSystem.Kernel(), *m_applicationProcess);
#endif
}

namespace
{
    constexpr char ACC_SAVE_AVATORS_BASE_PATH[] = "system/save/8000000000000010/su/avators";

    class IButtonMappingListImpl : public IButtonMappingList
    {
    public:
        explicit IButtonMappingListImpl(const std::unordered_map<NativeAnalogValues, Common::ParamPackage>& mappings)
        {
            m_indices.reserve(mappings.size());
            m_params.reserve(mappings.size());

            for (const auto& [index, param] : mappings)
            {
                m_indices.push_back(static_cast<uint32_t>(index));
                m_params.emplace_back(new IParamPackageImpl(param));
            }
        }
        explicit IButtonMappingListImpl(const std::unordered_map<NativeButtonValues, Common::ParamPackage>& mappings)
        {
            m_indices.reserve(mappings.size());
            m_params.reserve(mappings.size());

            for (const auto& [index, param] : mappings)
            {
                m_indices.push_back(static_cast<uint32_t>(index));
                m_params.emplace_back(new IParamPackageImpl(param));
            }
        }
        explicit IButtonMappingListImpl(const std::unordered_map<NativeMotionValues, Common::ParamPackage>& mappings)
        {
            m_indices.reserve(mappings.size());
            m_params.reserve(mappings.size());

            for (const auto& [index, param] : mappings)
            {
                m_indices.push_back(static_cast<uint32_t>(index));
                m_params.emplace_back(new IParamPackageImpl(param));
            }
        }

        ~IButtonMappingListImpl()
        {
            for (IParamPackageImpl* item : m_params)
            {
                item->Release();
            }
        }

        uint32_t GetCount() const override
        {
            return static_cast<uint32_t>(m_indices.size());
        }

        uint32_t GetIndex(uint32_t position) const override
        {
            return m_indices[position];
        }

        IParamPackage& GetParamPackage(uint32_t position) const override
        {
            return *m_params[position];
        }

        void Release() override
        {
            delete this;
        }

    private:
        std::vector<uint32_t> m_indices;
        std::vector<IParamPackageImpl*> m_params;
    };

    bool FillHostProfileInfo(const Service::Account::ProfileManager & manager, std::size_t index, HostProfileInfo * out_profile)
    {
        if (out_profile == nullptr)
        {
            return false;
        }

        const auto uuid = manager.GetUser(index);
        if (!uuid)
        {
            return false;
        }

        Service::Account::ProfileBase profile{};
        if (!manager.GetProfileBase(*uuid, profile))
        {
            return false;
        }

        std::memcpy(out_profile->uuid, profile.user_uuid.uuid.data(), HOST_PROFILE_UUID_SIZE);
        const std::string username = Common::StringFromFixedZeroTerminatedBuffer((const char *)profile.username.data(), profile.username.size());
        std::memset(out_profile->username, 0, sizeof(out_profile->username));
        std::strncpy(out_profile->username, username.c_str(), HOST_PROFILE_USERNAME_SIZE);
        return true;
    }

    std::filesystem::path ProfileImageFilesystemPath(const Common::UUID & uuid)
    {
        return Common::FS::GetYuzuPath(Common::FS::YuzuPath::NANDDir) / ACC_SAVE_AVATORS_BASE_PATH / (uuid.FormattedString() + ".jpg");
    }
}

extern IModuleSettings * g_settings;

OSManager::OSManager(ISystemModules & modules) :
    m_modules(modules),
    m_coreSystem(modules),
    m_applicationProcess(nullptr),
    m_programIndex(0),
    m_previousProgramIndex(-1),
    m_launchType(ApplicationLaunchType::FrontendInitiated)
{
}

OSManager::~OSManager()
{
    if (m_applicationProcess != nullptr)
    {
        m_applicationProcess->Close();
        m_applicationProcess = nullptr;
    }
}

void OSManager::EmulationStarting()
{
    g_settings->SetBool(NXOsSetting::UseSpeedLimit, true);

    m_emuThread = std::make_unique<EmuThread>(m_coreSystem, m_applicationProcess);
    m_emuThread->Start();
}

void OSManager::EmulationStopping(bool wait)
{
    g_settings->SetBool(NXOsSetting::UseSpeedLimit, true);
    g_settings->SetBool(NXCoreSetting::Has39BitAddressSpace, false);

    if (m_emuThread)
    {
        m_coreSystem.SetShuttingDown(true);
        if (m_coreSystem.IsPoweredOn())
        {
            m_coreSystem.SetExitRequested(true);
            m_coreSystem.GetAppletManager().RequestExit();
        }
        m_emuThread->Stop();
        if (wait)
        {
            m_emuThread.reset();
        }
    }
}

bool OSManager::Initialize(void)
{
    SetupOsSetting();
    m_coreSystem.Initialize();
    m_coreSystem.HIDCore().ReloadInputDevices();
    return true;
}

void OSManager::ShutDown()
{
    m_coreSystem.SetShuttingDown(true);
    if (m_coreSystem.IsPoweredOn())
    {
        m_coreSystem.SetExitRequested(true);
        m_coreSystem.GetAppletManager().RequestExit();
    }
    m_emuThread->SetRunning(true);
}

bool OSManager::IsShuttingDown() const
{
    return m_coreSystem.IsShuttingDown();
}

bool OSManager::IsPoweredOn() const
{
    return m_coreSystem.IsPoweredOn();
}

void OSManager::ShutdownMainProcess()
{
    m_coreSystem.ShutdownMainProcess();
}

bool OSManager::SetupCurrentProcess(uint64_t codeSize, const IProgramMetadata & metaData, uint64_t aslr_space_start, uint64_t & baseAddress, uint64_t & processID, bool is_hbl)
{
    if (m_applicationProcess == nullptr)
    {
        return CreateApplicationProcess(codeSize, metaData, aslr_space_start, baseAddress, processID, is_hbl);
    }
    Kernel::KProcess * const current = m_coreSystem.CurrentProcess();
    if (current == nullptr)
    {
        UNIMPLEMENTED();
        return false;
    }
    if (current->LoadFromMetadata(metaData, codeSize, aslr_space_start, is_hbl).IsError())
    {
        return false;
    }
    processID = current->GetProcessId();
    baseAddress = current->GetEntryPoint().GetValue();
    return true;
}

IKernelProcess * OSManager::CurrentProcess()
{
    Kernel::KProcess * current = m_coreSystem.CurrentProcess();
    if (current != nullptr)
    {
        return current;
    }
    return m_applicationProcess;
}

bool OSManager::CreateApplicationProcess(uint64_t codeSize, const IProgramMetadata & metaData, uint64_t aslr_space_start, uint64_t & baseAddress, uint64_t & processID, bool is_hbl)
{
    if (m_applicationProcess != nullptr)
    {
        return false;
    }
    m_coreSystem.InitializeKernel(metaData.GetTitleID(), metaData.Is64BitProgram());
    Kernel::KernelCore & kernel = m_coreSystem.Kernel();
    m_applicationProcess = Kernel::KProcess::Create(kernel);
    if (m_applicationProcess == nullptr)
    {
        return false;
    }
    Kernel::KProcess::Register(kernel, m_applicationProcess);
    kernel.AppendNewProcess(m_applicationProcess);
    kernel.MakeApplicationProcess(m_applicationProcess);

    if (m_applicationProcess->LoadFromMetadata(metaData, codeSize, aslr_space_start, is_hbl).IsError())
    {
        return false;
    }

    auto params = Service::AM::FrontendAppletParameters{
        .applet_id = Service::AM::AppletId::Application,
        .applet_type = Service::AM::AppletType::Application,
        .launch_type = m_launchType == ApplicationLaunchType::ApplicationInitiated
                           ? Service::AM::LaunchType::ApplicationInitiated
                           : Service::AM::LaunchType::FrontendInitiated,
        .program_index = m_programIndex,
        .previous_program_index = m_previousProgramIndex,
    };
    params.program_id = metaData.GetTitleID();
    m_coreSystem.GetAppletManager().CreateAndInsertByFrontendAppletParameters(
        m_applicationProcess->GetProcessId(), params, m_applicationProcess);

    m_programIndex = 0;
    m_previousProgramIndex = -1;
    m_launchType = ApplicationLaunchType::FrontendInitiated;

    processID = m_applicationProcess->GetProcessId();
    baseAddress = m_applicationProcess->GetEntryPoint().GetValue();
    return true;
}

void OSManager::StartApplicationProcess(int32_t priority, int64_t stackSize, uint32_t version, StorageId baseGameStorageId, StorageId updateStorageId, uint8_t * nacpData, uint32_t nacpDataLen)
{
#if NXEMU_ENABLE_PERF_CAPTURE_INSTRUMENTATION
    std::string display_version;
    // RawNACP starts with 16 language entries of 0x300 bytes each. The
    // version string follows the fixed 0x60-byte header after those entries.
    constexpr std::size_t version_offset = 0x3060;
    constexpr std::size_t version_size = 0x10;
    if (nacpData && nacpDataLen >= version_offset + version_size) {
        const auto* begin = reinterpret_cast<const char*>(nacpData + version_offset);
        display_version.assign(begin, std::find(begin, begin + version_size, '\0'));
    }
    m_performanceCapture.SetGameVersion(version, std::move(display_version));
#endif
    m_coreSystem.AddGlueRegistrationForProcess(*m_applicationProcess, version, baseGameStorageId, updateStorageId, nacpData, nacpDataLen);
    m_applicationProcess->Run(priority, stackSize);
    m_coreSystem.GetAppletManager().NotifyAppletStarted(m_applicationProcess->GetProcessId());
}

bool OSManager::LoadModule(const IModuleInfo & module, uint64_t baseAddress)
{
    Kernel::KProcess * const process = m_coreSystem.CurrentProcess() != nullptr ? m_coreSystem.CurrentProcess() : m_applicationProcess;
    if (process == nullptr)
    {
        return false;
    }
    process->LoadModule(module, baseAddress);
    return true;
}

IDeviceMemory & OSManager::DeviceMemory(void)
{
    return m_coreSystem.DeviceMemory();
}

void OSManager::KeyboardKeyPress(int modifier, int keyIndex, int keyCode)
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    input_subsystem->GetKeyboard()->SetKeyboardModifiers(modifier);
    input_subsystem->GetKeyboard()->PressKeyboardKey(keyIndex);
    input_subsystem->GetKeyboard()->PressKey(keyCode);
    input_subsystem->PumpEvents();
}

void OSManager::KeyboardKeyRelease(int modifier, int keyIndex, int keyCode)
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    input_subsystem->GetKeyboard()->SetKeyboardModifiers(modifier);
    input_subsystem->GetKeyboard()->ReleaseKeyboardKey(keyIndex);
    input_subsystem->GetKeyboard()->ReleaseKey(keyCode);
    input_subsystem->PumpEvents();
}

void OSManager::GatherGPUDirtyMemory(ICacheInvalidator * invalidator)
{
    m_coreSystem.GatherGPUDirtyMemory(invalidator);
}

uint64_t OSManager::GetGPUTicks()
{
    return m_coreSystem.CoreTiming().GetGPUTicks();
}

uint64_t OSManager::GetProgramId()
{
    return m_coreSystem.ApplicationProcess()->GetProgramId();
}

bool OSManager::GetExitLocked() const
{
    return m_coreSystem.GetExitLocked();
}

void OSManager::GameFrameEnd()
{
    m_coreSystem.GetPerfStats().EndGameFrame();
}

void OSManager::AudioGetSyncIDs(uint32_t * ids, uint32_t maxCount, uint32_t* actualCount)
{
    std::vector<AudioCore::Sink::AudioEngine> sinkIds = AudioCore::Sink::GetSinkIDs();
    if (actualCount)
    {
        *actualCount = (uint32_t)sinkIds.size();
    }

    if (ids != nullptr && maxCount > 0 && sinkIds.size() > 0)
    {
        memcpy(ids, sinkIds.data(), std::min(maxCount, (uint32_t)sinkIds.size()) * sizeof(uint32_t));
    }
}

void OSManager::AudioGetDeviceListForSink(uint32_t sinkId, bool capture, DeviceEnumCallback callback, void * userData)
{
    std::vector<std::string> devices = AudioCore::Sink::GetDeviceListForSink((AudioCore::Sink::AudioEngine)sinkId, capture);
    for (size_t i = 0, n = devices.size(); i < n; i++)
    {
        callback(devices[i].c_str(), userData);
    }
}

void OSManager::RegisterHostThread()
{
    m_coreSystem.RegisterHostThread();
}

IParamPackageList * OSManager::GetInputDevices() const
{
    return new IParamPackageListImpl(m_coreSystem.InputSubsystem()->GetInputDevices());
}

IEmulatedController & OSManager::GetEmulatedController(NpadIdType index)
{
    return *m_coreSystem.HIDCore().GetEmulatedController(index);
}

ButtonNames OSManager::GetButtonName(const IParamPackage& param) const
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    return input_subsystem->GetButtonName(param);
}

bool OSManager::IsController(const IParamPackage & params) const
{
    std::shared_ptr<InputCommon::InputSubsystem>& input_subsystem = m_coreSystem.InputSubsystem();
    return input_subsystem->IsController(params);
}

NpadStyleSet OSManager::GetSupportedStyleTag() const
{
    return m_coreSystem.HIDCore().GetSupportedStyleTag().raw;
}

IButtonMappingList * OSManager::GetButtonMappingForDevice(const IParamPackage & param) const
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    return new IButtonMappingListImpl(input_subsystem->GetButtonMappingForDevice(param));
}

IButtonMappingList * OSManager::GetAnalogMappingForDevice(const IParamPackage & param) const
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    return new IButtonMappingListImpl(input_subsystem->GetAnalogMappingForDevice(param));
}

IButtonMappingList * OSManager::GetMotionMappingForDevice(const IParamPackage & param) const
{
    std::shared_ptr<InputCommon::InputSubsystem>& input_subsystem = m_coreSystem.InputSubsystem();
    return new IButtonMappingListImpl(input_subsystem->GetMotionMappingForDevice(param));
}

void OSManager::BeginMapping(PollingInputType type)
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    input_subsystem->BeginMapping(type);
}

void OSManager::StopMapping()
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    input_subsystem->StopMapping();
}

IParamPackage * OSManager::GetNextInput() const
{
    std::shared_ptr<InputCommon::InputSubsystem> & input_subsystem = m_coreSystem.InputSubsystem();
    return new IParamPackageImpl(input_subsystem->GetNextInput());
}

void OSManager::PumpInputEvents() const
{
    std::shared_ptr<InputCommon::InputSubsystem>& input_subsystem = m_coreSystem.InputSubsystem();
    input_subsystem->PumpEvents();
}

PerfStatsResults OSManager::GetAndResetPerfStats()
{
    return m_coreSystem.GetAndResetPerfStats();
}

PerformanceCaptureSharedState& OSManager::GetPerformanceCaptureSharedState()
{
    return m_performanceCapture.SharedState();
}

void OSManager::SetPerformanceCaptureDevice(const char * model, const char * driver)
{
    m_performanceCapture.SetDevice(model, driver);
}

bool OSManager::IsPerformanceCaptureActive() const
{
    return m_performanceCapture.IsActive();
}

bool OSManager::StartPerformanceCapture(const PerformanceCaptureConfig & config)
{
    if (!m_coreSystem.IsPoweredOn() || IsEmulationPaused()) return false;
    auto effective_config = config;
    effective_config.cpu_hle_synchronization_active = m_coreSystem.CoreTiming().CpuHleSynchronizationEnabled();
    return m_performanceCapture.Start(GetProgramId(), effective_config, [this] { return m_coreSystem.CoreTiming().GetGlobalTimeUs(); });
}

bool OSManager::StopPerformanceCapture(char * output_path, uint32_t output_path_size)
{
    std::string path;
    if (!m_performanceCapture.Stop(
            [this] { return m_coreSystem.CoreTiming().GetGlobalTimeUs(); }, path))
    {
        return false;
    }
    if (output_path != nullptr && output_path_size != 0)
    {
        const std::size_t copy_size = std::min<std::size_t>(path.size(), output_path_size - 1);
        std::memcpy(output_path, path.data(), copy_size);
        output_path[copy_size] = '\0';
    }
    return true;
}

void OSManager::InvalidatePerformanceCapture(PerformanceInvalidation reason)
{
    m_performanceCapture.SharedState().Invalidate(reason);
}

void OSManager::SetEmulationPaused(bool paused)
{
    if (paused) InvalidatePerformanceCapture(PerformanceInvalidation::Paused);
    if (!m_emuThread)
    {
        return;
    }
    m_emuThread->SetRunning(!paused);
}

bool OSManager::IsEmulationPaused() const
{
    if (!m_emuThread)
    {
        return false;
    }
    return !m_emuThread->IsRunning();
}

void OSManager::SetFrontendApplets(ICabinetApplet * cabinet, IControllerApplet * controller, IErrorApplet * error, IMiiEditApplet * mii_edit, IParentalControlsApplet * parental_controls, IPhotoViewerApplet * photo_viewer, IProfileSelectApplet * profile_select, ISoftwareKeyboardApplet * software_keyboard, IWebBrowserApplet * web_browser)
{
    Service::AM::Frontend::FrontendAppletSet applets{};
    applets.cabinet = cabinet;
    applets.controller = controller;
    applets.error = error;
    applets.mii_edit = mii_edit;
    applets.parental_controls = parental_controls;
    applets.photo_viewer = photo_viewer;
    applets.profile_select = profile_select;
    applets.software_keyboard = software_keyboard;
    applets.web_browser = web_browser;
    m_coreSystem.SetFrontendAppletSet(std::move(applets));
}

void OSManager::SetPlayerButtonState(uint32_t player_index, uint32_t button_ordinal, bool pressed)
{
    InputCommon::VirtualGamepad* const virtual_gamepad = m_coreSystem.InputSubsystem()->GetVirtualGamepad();
    if (virtual_gamepad == nullptr)
    {
        return;
    }
    virtual_gamepad->SetButtonState(player_index, static_cast<int>(button_ordinal), pressed);
}

void OSManager::SetPlayerAnalogState(uint32_t player_index, uint32_t stick_index, float x, float y)
{
    InputCommon::VirtualGamepad* const virtual_gamepad = m_coreSystem.InputSubsystem()->GetVirtualGamepad();
    if (virtual_gamepad == nullptr)
    {
        return;
    }
    virtual_gamepad->SetStickPosition(player_index, static_cast<int>(stick_index), x, y);
}

uint32_t OSManager::GetProfileCount() const
{
    Service::Account::ProfileManager manager;
    return (uint32_t)manager.GetUserCount();
}

bool OSManager::GetProfile(uint32_t index, HostProfileInfo * out_profile) const
{
    Service::Account::ProfileManager manager;
    return FillHostProfileInfo(manager, index, out_profile);
}

bool OSManager::CreateProfile(const uint8_t uuid_bytes[HOST_PROFILE_UUID_SIZE], const char * username_utf8, HostProfileInfo * out_profile)
{
    if (uuid_bytes == nullptr || username_utf8 == nullptr || username_utf8[0] == '\0')
    {
        return false;
    }

    Service::Account::ProfileManager manager;
    if (manager.GetUserCount() >= Service::Account::MAX_USERS)
    {
        return false;
    }

    std::array<uint8_t, HOST_PROFILE_UUID_SIZE> uuid_array{};
    std::memcpy(uuid_array.data(), uuid_bytes, HOST_PROFILE_UUID_SIZE);
    const Common::UUID uuid{uuid_array};
    if (uuid.IsInvalid() || manager.UserExists(uuid))
    {
        return false;
    }

    if (manager.CreateNewUser(uuid, std::string(username_utf8)).IsError())
    {
        return false;
    }

    manager.WriteUserSaveFile();
    if (out_profile == nullptr)
    {
        return true;
    }

    const auto index = manager.GetUserIndex(uuid);
    return index && FillHostProfileInfo(manager, *index, out_profile);
}

bool OSManager::RenameProfile(const uint8_t uuid_bytes[HOST_PROFILE_UUID_SIZE], const char * username_utf8)
{
    if (uuid_bytes == nullptr || username_utf8 == nullptr || username_utf8[0] == '\0')
    {
        return false;
    }

    std::array<uint8_t, HOST_PROFILE_UUID_SIZE> uuid_array{};
    std::memcpy(uuid_array.data(), uuid_bytes, HOST_PROFILE_UUID_SIZE);
    const Common::UUID uuid{uuid_array};

    Service::Account::ProfileManager manager;
    Service::Account::ProfileBase profile{};
    if (!manager.GetProfileBase(uuid, profile))
    {
        return false;
    }

    const std::string username(username_utf8);
    profile.username.fill(0);
    std::copy_n(username.begin(), std::min(username.size(), profile.username.size()), profile.username.begin());

    if (!manager.SetProfileBase(uuid, profile))
    {
        return false;
    }

    manager.WriteUserSaveFile();
    return true;
}

bool OSManager::RemoveProfile(const uint8_t uuid_bytes[HOST_PROFILE_UUID_SIZE])
{
    if (uuid_bytes == nullptr)
    {
        return false;
    }

    std::array<uint8_t, HOST_PROFILE_UUID_SIZE> uuid_array{};
    std::memcpy(uuid_array.data(), uuid_bytes, HOST_PROFILE_UUID_SIZE);
    const Common::UUID uuid{uuid_array};

    Service::Account::ProfileManager manager;
    if (manager.GetUserCount() < 2 || !manager.RemoveUser(uuid))
    {
        return false;
    }

    manager.WriteUserSaveFile();
    return true;
}

bool OSManager::SetProfileImage(const uint8_t uuid_bytes[HOST_PROFILE_UUID_SIZE], const uint8_t * image_data, uint32_t image_size)
{
    if (uuid_bytes == nullptr || image_data == nullptr || image_size == 0)
    {
        return false;
    }

    std::array<uint8_t, HOST_PROFILE_UUID_SIZE> uuid_array{};
    std::memcpy(uuid_array.data(), uuid_bytes, HOST_PROFILE_UUID_SIZE);
    const Common::UUID uuid{uuid_array};

    Service::Account::ProfileManager manager;
    if (!manager.UserExists(uuid))
    {
        return false;
    }

    return WriteProfileJpegFromMemory(image_data, image_size, ProfileImageFilesystemPath(uuid));
}

bool OSManager::GetProfileImagePath(const uint8_t uuid_bytes[HOST_PROFILE_UUID_SIZE], char * out_path, uint32_t out_path_size) const
{
    if (uuid_bytes == nullptr || out_path == nullptr || out_path_size == 0)
    {
        return false;
    }

    std::array<uint8_t, HOST_PROFILE_UUID_SIZE> uuid_array{};
    std::memcpy(uuid_array.data(), uuid_bytes, HOST_PROFILE_UUID_SIZE);
    const Common::UUID uuid{uuid_array};

    Service::Account::ProfileManager manager;
    if (!manager.UserExists(uuid))
    {
        return false;
    }

    const std::filesystem::path imagePath = ProfileImageFilesystemPath(uuid);
    std::error_code ec;
    if (!std::filesystem::exists(imagePath, ec) || ec)
    {
        out_path[0] = '\0';
        return true;
    }

    const std::string path = Common::FS::PathToUTF8String(imagePath);
    if (path.size() + 1 > out_path_size)
    {
        return false;
    }

    std::memcpy(out_path, path.c_str(), path.size() + 1);
    return true;
}

void OSManager::SetApplicationLaunchParameters(int32_t program_index, int32_t previous_program_index, ApplicationLaunchType launch_type)
{
    m_programIndex = program_index;
    m_previousProgramIndex = previous_program_index;
    m_launchType = launch_type;
}

void OSManager::RegisterExecuteProgramCallback(ExecuteProgramCallback callback, void * userData)
{
    m_coreSystem.RegisterExecuteProgramCallback(callback, userData);
}

void OSManager::RegisterExitCallback(ExitCallback callback, void * userData)
{
    m_coreSystem.RegisterExitCallback(callback, userData);
}

void OSManager::ExportUserChannel(UserChannelEntryCallback callback, void * userData)
{
    if (callback == nullptr)
    {
        return;
    }
    for (const auto & entry : m_coreSystem.GetUserChannel())
    {
        callback(entry.data(), (uint32_t)entry.size(), userData);
    }
}

void OSManager::PushUserChannelEntry(const uint8_t * data, uint32_t size)
{
    if (data == nullptr || size == 0)
    {
        m_coreSystem.GetUserChannel().emplace_back();
        return;
    }
    m_coreSystem.GetUserChannel().emplace_back(data, data + size);
}
