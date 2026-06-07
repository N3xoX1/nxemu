#include "os_manager.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/perf_stats.h"
#include "os_settings.h"
#include "yuzu_audio_core/sink/sink_details.h"
#include "yuzu_common/settings.h"
#include "yuzu_common/settings_input.h"
#include "yuzu_hid_core/frontend/emulated_controller.h"
#include "yuzu_hid_core/hid_core.h"
#include "yuzu_input_common/drivers/keyboard.h"
#include "yuzu_input_common/drivers/virtual_gamepad.h"
#include "yuzu_input_common/main.h"
#include <nxemu-core/settings/identifiers.h>

namespace
{
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
}

extern IModuleSettings * g_settings;

OSManager::OSManager(ISystemModules & modules) :
    m_modules(modules),
    m_coreSystem(modules),
    m_process(nullptr)
{
}

OSManager::~OSManager()
{
    if (m_process != nullptr)
    {
        m_process->Close();
        m_process = nullptr;
    }
}

void OSManager::EmulationStarting()
{
    m_emuThread = std::make_unique<EmuThread>(m_coreSystem, m_process);
    m_emuThread->Start();
}

void OSManager::EmulationStopping(bool wait)
{
    // Disable unlimited frame rate
    Settings::values.use_speed_limit.SetValue(true);

    if (m_emuThread)
    {
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

bool OSManager::CreateApplicationProcess(uint64_t codeSize, const IProgramMetadata & metaData, uint64_t & baseAddress, uint64_t & processID, bool is_hbl)
{
    if (m_process != nullptr)
    {
        return false;
    }
    m_coreSystem.InitializeKernel(metaData.GetTitleID());
    Kernel::KernelCore & kernel = m_coreSystem.Kernel();
    m_process = Kernel::KProcess::Create(kernel);
    if (m_process == nullptr)
    {
        return false;
    }
    Kernel::KProcess::Register(kernel, m_process);
    kernel.AppendNewProcess(m_process);
    kernel.MakeApplicationProcess(m_process);

    if (m_process->LoadFromMetadata(metaData, codeSize, 0, false).IsError())
    {
        return false;
    }
    
    auto params = Service::AM::FrontendAppletParameters{
        .applet_id = Service::AM::AppletId::Application,
        .applet_type = Service::AM::AppletType::Application,
        .launch_type = Service::AM::LaunchType::FrontendInitiated,
    };
    params.program_id = metaData.GetTitleID();
    m_coreSystem.GetAppletManager().CreateAndInsertByFrontendAppletParameters(m_process->GetProcessId(), params);

    processID = m_process->GetProcessId();
    baseAddress = GetInteger(m_process->GetEntryPoint());
    return true;
}

void OSManager::StartApplicationProcess(int32_t priority, int64_t stackSize, uint32_t version, StorageId baseGameStorageId, StorageId updateStorageId, uint8_t * nacpData, uint32_t nacpDataLen)
{
    m_coreSystem.AddGlueRegistrationForProcess(*m_process, version, baseGameStorageId, updateStorageId, nacpData, nacpDataLen);
    m_process->Run(priority, stackSize);
}

bool OSManager::LoadModule(const IModuleInfo & module, uint64_t baseAddress)
{
    if (m_process == nullptr)
    {
        return false;
    }
    m_process->LoadModule(module, baseAddress);
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
    std::vector<Settings::AudioEngine> sinkIds = AudioCore::Sink::GetSinkIDs();
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
    std::vector<std::string> devices = AudioCore::Sink::GetDeviceListForSink((Settings::AudioEngine)sinkId, capture);
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

void OSManager::SetEmulationPaused(bool paused)
{
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

void OSManager::SetFrontendApplets(ICabinetFrontendApplet * cabinet, IControllerFrontendApplet * controller, IErrorFrontendApplet * error, IMiiEditFrontendApplet * mii_edit, IParentalControlsFrontendApplet * parental_controls, IPhotoViewerFrontendApplet * photo_viewer, IProfileSelectFrontendApplet * profile_select, ISoftwareKeyboardFrontendApplet * software_keyboard, IWebBrowserFrontendApplet * web_browser)
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