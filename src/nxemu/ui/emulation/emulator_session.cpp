#include "emulator_session.h"
#include <chrono>
#include <filesystem>
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-module-spec/video.h>
#include <nxemu/ui/host/sdl_input.h>
#include <nxemu/ui/host/ui_notification.h>
#include <nxemu/ui/i18n/translation.h>
#include <yuzu_common/fs/filesystem_interfaces.h>
#include <yuzu_common/fs/fs_util.h>
#include <yuzu_common/logging/log.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#endif

EmulatorSession::EmulatorSession(IRenderWindow & renderWindow, UiNotification & notification, SDL_Window * window) :
    m_renderWindow(renderWindow),
    m_notification(notification),
    m_window(window)
{
}

EmulatorSession::~EmulatorSession()
{
    m_notification.Close();
    if (m_modules.IsValid())
    {
        SettingsStore::GetInstance().SetBool(NXCoreSetting::ShuttingDown, true);
    }
    m_modules.ShutDown();
}

bool EmulatorSession::Initialise()
{
    m_notification.BindWindow(m_window);
    m_modules.Setup(m_renderWindow);
    if (!m_modules.IsValid())
    {
        return false;
    }
    RegisterSystemCallbacks();
    return true;
}

void EmulatorSession::FlushSettings()
{
    if (m_modules.IsValid())
    {
        m_modules.FlushSettings();
    }
}

bool EmulatorSession::LoadGame(const std::string & path)
{
    if (IsRunning())
    {
        ReportError(Tr("Stop the current game before opening another one."));
        return false;
    }
    return LoadProgram(path, 0, -1, ApplicationLaunchType::FrontendInitiated, {});
}

void EmulatorSession::Stop()
{
    if (m_modules.IsLaunching())
    {
        m_launchCancelled = true;
        m_modules.CancelLaunch();
        return;
    }
    if (!IsRunning())
    {
        return;
    }
    m_currentProgramIndex = 0;
    m_previousProgramIndex = -1;
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
}

void EmulatorSession::TogglePause()
{
    if (!m_modules.IsValid() || !IsRunning())
    {
        return;
    }
    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    os.SetEmulationPaused(!os.IsEmulationPaused());
}

bool EmulatorSession::IsRunning() const
{
    return m_modules.IsLaunching() || (m_modules.IsValid() && SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning));
}

bool EmulatorSession::IsLaunching() const
{
    return m_modules.IsLaunching();
}

bool EmulatorSession::IsPaused()
{
    return m_modules.IsValid() && IsRunning() && m_modules.Modules().OperatingSystem().IsEmulationPaused();
}

EmulatorSession::Phase EmulatorSession::CurrentPhase() const
{
    if (m_modules.IsLaunching())
    {
        return Phase::Loading;
    }
    SettingsStore & settings = SettingsStore::GetInstance();
    switch (static_cast<EmulationState>(settings.GetInt(NXCoreSetting::EmulationState)))
    {
    case EmulationState::Starting:
    case EmulationState::LoadingRom:
    case EmulationState::RomLoaded:
        return Phase::Loading;
    case EmulationState::Running:
        return settings.GetBool(NXCoreSetting::DisplayedFrames) ? Phase::Running : Phase::Loading;
    case EmulationState::Paused:
        return Phase::Paused;
    default:
        return Phase::Idle;
    }
}

void EmulatorSession::Update(bool releaseStoppedResources)
{
    m_notification.Pump();
    if (m_modules.IsLaunching())
    {
        if (m_modules.LaunchDone())
        {
            FinishLaunch();
        }
        return;
    }
    ProcessProgramEvents();
    if (IsRunning() && SettingsStore::GetInstance().GetInt(NXCoreSetting::GpuInitializationState) == static_cast<int32_t>(GpuInitializationState::Failed))
    {
        Stop();
        ReportError(Tr("The graphics renderer could not be initialized. Check that your GPU drivers are up to date."));
    }
    if (releaseStoppedResources && m_releasePending && !IsRunning() &&
        SettingsStore::GetInstance().GetInt(NXCoreSetting::EmulationState) == static_cast<int32_t>(EmulationState::Stopped))
    {
        if (!ResetModules())
        {
            ReportError(Tr("Required modules failed to reload after stopping the game."));
            return;
        }
        SettingsStore::GetInstance().SetInt(NXCoreSetting::EmulationState, static_cast<int32_t>(EmulationState::Stopped));
#ifdef __APPLE__
        m_idleTrimAt = SDL_GetTicks() + 1000;
#endif
    }
#ifdef __APPLE__
    if (m_idleTrimAt != 0 && releaseStoppedResources && !IsRunning() && SDL_GetTicks() >= m_idleTrimAt)
    {
        m_idleTrimAt = 0;
        malloc_zone_pressure_relief(nullptr, 0);
    }
#endif
    if (m_modules.IsValid())
    {
        m_modules.Modules().OperatingSystem().PumpInputEvents();
    }
}

void EmulatorSession::HandleKey(const SDL_KeyboardEvent & event)
{
    if (m_modules.IsValid())
    {
        ForwardKeyboardEvent(event, m_modules.Modules().OperatingSystem());
    }
}

void EmulatorSession::SetViewportSize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || (width == m_viewportWidth && height == m_viewportHeight))
    {
        return;
    }
    m_viewportWidth = width;
    m_viewportHeight = height;
    if (m_modules.IsValid())
    {
        m_modules.Modules().Video().UpdateFramebufferLayout(width, height);
    }
}

bool EmulatorSession::LoadProgram(const std::string & path, int32_t programIndex, int32_t previousProgramIndex,
                                  ApplicationLaunchType launchType, const UserChannel & userChannel)
{
    const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::path{Common::FS::ToU8String(path)}, error))
    {
        ReportError(Tr("The game file does not exist:\n%1", {path}));
        return false;
    }
    uint64_t programId = 0;
    if (m_modules.IsValid())
    {
        // LoadRom applies programIndex to the ID, so resolve the base program here.
        IRomInfoPtr info(m_modules.Modules().Systemloader().RomInfo(path.c_str(), 0, 0));
        if (info)
        {
            info->ReadProgramId(programId);
        }
    }
    ReleaseModules();
    m_pendingLaunch = PendingLaunch{path, programId, programIndex, previousProgramIndex, launchType, userChannel, started};
    m_launchCancelled = false;
    m_notification.BindWindow(m_window);
    if (!m_modules.StartLaunch(m_renderWindow, PrepareLaunch, this))
    {
        ReportError(Tr("Required modules failed to load."));
        return false;
    }
    m_releasePending = true;
    LOG_INFO(Frontend, "Game launch: load modules in {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    return true;
}

bool EmulatorSession::PrepareLaunch(void * userData)
{
    EmulatorSession & session = *static_cast<EmulatorSession *>(userData);
    const PendingLaunch & launch = session.m_pendingLaunch;
    ISystemModules & modules = session.m_modules.Modules();
    for (const std::vector<uint8_t> & entry : launch.userChannel)
    {
        modules.OperatingSystem().PushUserChannelEntry(entry.data(), static_cast<uint32_t>(entry.size()));
    }
    return modules.Systemloader().PrepareRom(launch.path.c_str(), launch.programId, launch.programIndex, launch.previousProgramIndex, launch.launchType);
}

void EmulatorSession::FinishLaunch()
{
    const bool prepared = m_modules.FinishLaunch();
    const PendingLaunch launch = std::move(m_pendingLaunch);
    const std::chrono::steady_clock::time_point preparedAt = std::chrono::steady_clock::now();
    LOG_INFO(Frontend, "Game launch: prepared in {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(preparedAt - launch.started).count());
    if (m_launchCancelled || !prepared)
    {
        AbortLaunch(m_launchCancelled ? std::string() : LoadFailureMessage(launch.path));
        return;
    }

    m_modules.Modules().OperatingSystem().InitializeInput();
    RegisterSystemCallbacks();
    if (m_viewportWidth != 0 && m_viewportHeight != 0)
    {
        m_modules.Modules().Video().UpdateFramebufferLayout(m_viewportWidth, m_viewportHeight);
    }
    if (m_listener != nullptr)
    {
        m_listener->OnModulesReady(true);
    }
    if (!m_modules.Modules().Systemloader().StartPreparedRom())
    {
        const bool gpuFailed = SettingsStore::GetInstance().GetInt(NXCoreSetting::GpuInitializationState) == static_cast<int32_t>(GpuInitializationState::Failed);
        AbortLaunch(gpuFailed ? Tr("The graphics renderer could not be initialized. Check that your GPU drivers are up to date.") : LoadFailureMessage(launch.path));
        return;
    }
    const std::chrono::steady_clock::time_point loaded = std::chrono::steady_clock::now();
    LOG_INFO(Frontend, "Game launch: start in {} ms, total {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(loaded - preparedAt).count(),
             std::chrono::duration_cast<std::chrono::milliseconds>(loaded - launch.started).count());
    m_currentProgramIndex = launch.programIndex;
    m_previousProgramIndex = launch.previousProgramIndex;
    if (m_listener != nullptr)
    {
        m_listener->OnGameStarted(launch.path);
    }
}

void EmulatorSession::AbortLaunch(const std::string & message)
{
    if (!ResetModules())
    {
        ReportError(Tr("Required modules failed to reload after stopping the game."));
        return;
    }
    SettingsStore::GetInstance().SetInt(NXCoreSetting::EmulationState, static_cast<int32_t>(EmulationState::Stopped));
    if (!message.empty())
    {
        ReportError(message);
    }
}

std::string EmulatorSession::LoadFailureMessage(const std::string & path) const
{
    const LoaderResultStatus result = static_cast<LoaderResultStatus>(SettingsStore::GetInstance().GetInt(NXCoreSetting::GameLoadResult));
    if (result == LoaderResultStatus::ErrorNoExeFS)
    {
        return Tr("This game has no usable executable (ExeFS).\n\nIf this copy requires an update, add its folder in Settings > Game directories, then enable the update in the game's details. If an update is already enabled, check that its files are complete and readable.\n\nGame: %1", {path});
    }
    if (result != LoaderResultStatus::ErrorNotInitialized)
    {
        return Tr("Could not load the game:\n%1\n\nLoader error: %2. Check that the game files are complete and that the required keys and firmware are available.", {path, std::to_string(static_cast<int>(result))});
    }
    return Tr("Could not load the game:\n%1", {path});
}

void EmulatorSession::ReleaseModules()
{
    m_releasePending = false;
#ifdef __APPLE__
    m_idleTrimAt = 0;
#endif
    if (m_listener != nullptr)
    {
        m_listener->OnModulesReleasing();
    }
    m_notification.Close();
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    m_modules.ShutDown();
    m_callbackContext.reset();
    ++m_moduleGeneration;
    {
        std::lock_guard<std::mutex> lock(m_programEventsMutex);
        m_programEvents.clear();
    }
}

bool EmulatorSession::ResetModules()
{
    ReleaseModules();
    m_modules.Setup(m_renderWindow);
    m_notification.BindWindow(m_window);
    if (!m_modules.IsValid())
    {
        return false;
    }
    RegisterSystemCallbacks();
    if (m_viewportWidth != 0 && m_viewportHeight != 0)
    {
        m_modules.Modules().Video().UpdateFramebufferLayout(m_viewportWidth, m_viewportHeight);
    }
    if (m_listener != nullptr)
    {
        m_listener->OnModulesReady(false);
    }
    return true;
}

void EmulatorSession::RegisterSystemCallbacks()
{
    m_callbackContext = std::make_unique<CallbackContext>(CallbackContext{this, ++m_moduleGeneration});
    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    os.RegisterExecuteProgramCallback(ExecuteProgramCallback, m_callbackContext.get());
    os.RegisterExitCallback(ExitProgramCallback, m_callbackContext.get());
}

void EmulatorSession::ProcessProgramEvents()
{
    std::deque<ProgramEvent> events;
    {
        std::lock_guard<std::mutex> lock(m_programEventsMutex);
        events.swap(m_programEvents);
    }
    for (const ProgramEvent & event : events)
    {
        if (event.generation != m_moduleGeneration)
        {
            continue;
        }
        const std::string path = SettingsStore::GetInstance().GetString(NXCoreSetting::GameFile);
        if (event.type == ProgramEvent::Type::Execute)
        {
            LoadProgram(path, static_cast<int32_t>(event.index), m_currentProgramIndex, ApplicationLaunchType::ApplicationInitiated, event.userChannel);
        }
        else if (m_currentProgramIndex != 0)
        {
            const int32_t previous = m_previousProgramIndex >= 0 ? m_previousProgramIndex : 0;
            LoadProgram(path, previous, m_currentProgramIndex, ApplicationLaunchType::ApplicationInitiated, {});
        }
        else
        {
            Stop();
        }
    }
}

void EmulatorSession::ReportError(const std::string & message)
{
    if (m_listener != nullptr)
    {
        m_listener->OnSessionError(message);
    }
}

void EmulatorSession::CollectUserChannel(const uint8_t * data, uint32_t size, void * userData)
{
    UserChannel & channel = *static_cast<UserChannel *>(userData);
    if (data != nullptr && size != 0)
    {
        channel.emplace_back(data, data + size);
    }
    else
    {
        channel.emplace_back();
    }
}

void EmulatorSession::ExecuteProgramCallback(uint32_t index, void * userData)
{
    const CallbackContext & context = *static_cast<CallbackContext *>(userData);
    EmulatorSession & session = *context.session;
    ProgramEvent event{ProgramEvent::Type::Execute, context.generation, index, {}};
    session.m_modules.Modules().OperatingSystem().ExportUserChannel(CollectUserChannel, &event.userChannel);
    std::lock_guard<std::mutex> lock(session.m_programEventsMutex);
    session.m_programEvents.push_back(std::move(event));
}

void EmulatorSession::ExitProgramCallback(void * userData)
{
    const CallbackContext & context = *static_cast<CallbackContext *>(userData);
    EmulatorSession & session = *context.session;
    std::lock_guard<std::mutex> lock(session.m_programEventsMutex);
    session.m_programEvents.push_back({ProgramEvent::Type::Exit, context.generation, 0, {}});
}
