#include "os_manager.h"
#include "profile_image_writer.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/k_process.h"
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
#include <filesystem>

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
    m_applicationProcess(nullptr)
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

bool OSManager::SetupCurrentProcess(uint64_t codeSize, const IProgramMetadata & metaData, uint64_t & baseAddress, uint64_t & processID, bool is_hbl)
{
    if (m_applicationProcess == nullptr)
    {
        return CreateApplicationProcess(codeSize, metaData, baseAddress, processID, is_hbl);
    }
    Kernel::KProcess * const current = m_coreSystem.CurrentProcess();
    if (current == nullptr)
    {
        UNIMPLEMENTED();
        return false;
    }
    if (current->LoadFromMetadata(metaData, codeSize, 0, is_hbl).IsError())
    {
        return false;
    }
    processID = current->GetProcessId();
    baseAddress = GetInteger(current->GetEntryPoint());
    return true;
}

bool OSManager::CreateApplicationProcess(uint64_t codeSize, const IProgramMetadata & metaData, uint64_t & baseAddress, uint64_t & processID, bool is_hbl)
{
    if (m_applicationProcess != nullptr)
    {
        return false;
    }
    m_coreSystem.InitializeKernel(metaData.GetTitleID());
    Kernel::KernelCore & kernel = m_coreSystem.Kernel();
    m_applicationProcess = Kernel::KProcess::Create(kernel);
    if (m_applicationProcess == nullptr)
    {
        return false;
    }
    Kernel::KProcess::Register(kernel, m_applicationProcess);
    kernel.AppendNewProcess(m_applicationProcess);
    kernel.MakeApplicationProcess(m_applicationProcess);
    g_settings->SetBool(NXCoreSetting::Has39BitAddressSpace, metaData.GetAddressSpaceType() == ProgramAddressSpaceType::Is39Bit);

    if (m_applicationProcess->LoadFromMetadata(metaData, codeSize, 0, is_hbl).IsError())
    {
        return false;
    }

    auto params = Service::AM::FrontendAppletParameters{
        .applet_id = Service::AM::AppletId::Application,
        .applet_type = Service::AM::AppletType::Application,
        .launch_type = Service::AM::LaunchType::FrontendInitiated,
    };
    params.program_id = metaData.GetTitleID();
    m_coreSystem.GetAppletManager().CreateAndInsertByFrontendAppletParameters(m_applicationProcess->GetProcessId(), params);

    processID = m_applicationProcess->GetProcessId();
    baseAddress = GetInteger(m_applicationProcess->GetEntryPoint());
    return true;
}

void OSManager::StartApplicationProcess(int32_t priority, int64_t stackSize, uint32_t version, StorageId baseGameStorageId, StorageId updateStorageId, uint8_t * nacpData, uint32_t nacpDataLen)
{
    m_coreSystem.AddGlueRegistrationForProcess(*m_applicationProcess, version, baseGameStorageId, updateStorageId, nacpData, nacpDataLen);
    m_applicationProcess->Run(priority, stackSize);
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
