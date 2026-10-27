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
    if (m_loading)
    {
        // Services started by the load are only torn down safely by the regular emulation stop.
        Loader().CancelPrepareRom();
        m_loadThread.join();
        m_loading = false;
        if (m_loadResult)
        {
            Loader().StartPreparedRom();
        }
    }
    if (m_modules.IsValid())
    {
        SettingsStore::GetInstance().SetBool(NXCoreSetting::ShuttingDown, true);
    }
    m_inputService.Detach();
    m_profileService.Detach();
    m_modules.ShutDown();
}

HostProfileService & EmulatorSession::Profiles()
{
    return m_profileService;
}

bool EmulatorSession::Initialise()
{
    m_notification.BindWindow(m_window);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    // The frontend can show existing profiles before loading guest OS modules.
    // The authoritative OS snapshot replaces this view when Setup succeeds.
    m_profileService.RefreshOffline();
    const auto setup_start = std::chrono::steady_clock::now();
    const bool ready = m_modules.DisplayReady() ? m_modules.CompleteFrontendSetup() : m_modules.SetupFrontend(m_renderWindow);
    LOG_DEBUG(Frontend, "Frontend module setup: {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - setup_start).count());
    return ready;
}

bool EmulatorSession::InitialiseDisplay()
{
    m_notification.BindWindow(m_window);
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    return m_modules.SetupDisplay(m_renderWindow);
}

void EmulatorSession::FlushSettings()
{
    m_modules.FlushSettings();
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
    if (m_loading)
    {
        // The load stops at its next safe point; FinishLoad then releases the session instead of starting it.
        m_pendingLaunch.cancelled = true;
        Loader().CancelPrepareRom();
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
    if (m_loading || !IsRunning())
    {
        return;
    }
    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    os.SetEmulationPaused(!os.IsEmulationPaused());
}

bool EmulatorSession::IsRunning() const
{
    return m_loading || IsEmulating();
}

bool EmulatorSession::IsEmulating() const
{
    return !m_loading && m_modules.IsValid() && SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning);
}

bool EmulatorSession::IsPaused()
{
    return IsEmulating() && m_modules.Modules().OperatingSystem().IsEmulationPaused();
}

EmulatorSession::Phase EmulatorSession::CurrentPhase() const
{
    if (m_loading)
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
    if (m_loading)
    {
        if (m_loadDone.load(std::memory_order_acquire))
        {
            FinishLoad();
        }
        return;
    }
    ProcessProgramEvents();
    if (releaseStoppedResources && m_releasePending && !IsRunning() &&
        SettingsStore::GetInstance().GetInt(NXCoreSetting::EmulationState) == static_cast<int32_t>(EmulationState::Stopped))
    {
        ReleaseSession();
    }
#ifdef __APPLE__
    if (m_idleTrimAt != 0 && releaseStoppedResources && !IsRunning() && SDL_GetTicks64() >= m_idleTrimAt)
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
    const auto started = std::chrono::steady_clock::now();
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::path{Common::FS::ToU8String(path)}, error))
    {
        ReportError(Tr("The game file does not exist:\n%1", {path}));
        return false;
    }
    if (!PrepareSession())
    {
        ReportError(Tr("Required modules failed to load."));
        return false;
    }
    const auto resetDone = std::chrono::steady_clock::now();
    LOG_INFO(Frontend, "Game launch: prepare guest session in {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(resetDone - started).count());
    // All catalogue workers have been joined by OnModulesReleasing.
    uint64_t programId = 0;
    if (IRomInfoPtr info(Loader().RomInfo(path.c_str(), 0, programIndex)); info)
    {
        info->ReadProgramId(programId);
    }
    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    m_releasePending = true;
    for (const std::vector<uint8_t> & entry : userChannel)
    {
        os.PushUserChannelEntry(entry.data(), static_cast<uint32_t>(entry.size()));
    }
    // Reading the game can take seconds on a cold disk; keep the UI responsive meanwhile.
    m_pendingLaunch = PendingLaunch{path, programIndex, previousProgramIndex, started, resetDone};
    m_loadDone.store(false, std::memory_order_relaxed);
    m_loading = true;
    SettingsStore::GetInstance().SetInt(NXCoreSetting::EmulationState, static_cast<int32_t>(EmulationState::LoadingRom));
    m_loadThread = std::thread([this, path, programId, programIndex, previousProgramIndex, launchType] {
        m_loadResult = Loader().PrepareRom(path.c_str(), programId, programIndex, previousProgramIndex, launchType);
        m_loadDone.store(true, std::memory_order_release);
    });
    return true;
}

void EmulatorSession::FinishLoad()
{
    m_loadThread.join();
    m_loading = false;
    const PendingLaunch launch = std::move(m_pendingLaunch);
    if (launch.cancelled)
    {
        LOG_INFO(Frontend, "Game launch cancelled while loading");
        // A load that got past its last cancel point already started guest services, which only
        // the regular emulation stop tears down safely: start the game, then stop it like a user stop.
        if (m_loadResult && Loader().StartPreparedRom())
        {
            SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
        }
        else
        {
            ReleaseSession();
        }
        return;
    }
    if (!m_loadResult || !Loader().StartPreparedRom())
    {
        ReportLoadFailure(launch.path);
        return;
    }
    const auto loaded = std::chrono::steady_clock::now();
    LOG_INFO(Frontend, "Game launch: load ROM in {} ms, total {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(loaded - launch.resetDone).count(),
             std::chrono::duration_cast<std::chrono::milliseconds>(loaded - launch.started).count());
    m_currentProgramIndex = launch.programIndex;
    m_previousProgramIndex = launch.previousProgramIndex;
    if (m_listener != nullptr)
    {
        m_listener->OnGameStarted(launch.path);
    }
}

void EmulatorSession::ReportLoadFailure(const std::string & path)
{
    ReleaseSession();
    const LoaderResultStatus result = static_cast<LoaderResultStatus>(SettingsStore::GetInstance().GetInt(NXCoreSetting::GameLoadResult));
    if (result == LoaderResultStatus::ErrorNoExeFS)
    {
        ReportError(Tr("This game has no usable executable (ExeFS).\n\nIf this copy requires an update, add its folder in Settings > Game directories, then enable the update in the game's details. If an update is already enabled, check that its files are complete and readable.\n\nGame: %1", {path}));
    }
    else if (result != LoaderResultStatus::ErrorNotInitialized)
    {
        ReportError(Tr("Could not load the game:\n%1\n\nLoader error: %2. Check that the game files are complete and that the required keys and firmware are available.", {path, std::to_string(static_cast<int>(result))}));
    }
    else
    {
        ReportError(Tr("Could not load the game:\n%1", {path}));
    }
}

bool EmulatorSession::PrepareSession()
{
    const auto start = std::chrono::steady_clock::now();
    m_releasePending = false;
#ifdef __APPLE__
    m_idleTrimAt = 0;
#endif
    if (m_listener)
    {
        m_listener->OnModulesReleasing(); // Drain catalogue/firmware workers before loading.
    }
    m_notification.Close();
    m_inputService.Detach();
    m_profileService.Detach();
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    m_modules.StopSession();
    m_callbackContext.reset();
    {
        std::lock_guard<std::mutex> lock(m_programEventsMutex);
        m_programEvents.clear();
    }
    if (!m_modules.StartSession())
    {
        m_profileService.RefreshOffline();
        SettingsStore::GetInstance().SetInt(NXCoreSetting::EmulationState, static_cast<int32_t>(EmulationState::Stopped));
        m_notification.BindWindow(m_window);
        if (m_listener) { m_listener->OnModulesReady(); }
        return false;
    }
    RegisterSystemCallbacks();
    m_profileService.Attach(m_modules.Modules().OperatingSystem());
    m_inputService.Attach(m_modules.Modules().OperatingSystem());
    if (m_viewportWidth && m_viewportHeight)
    {
        m_modules.Modules().Video().UpdateFramebufferLayout(m_viewportWidth, m_viewportHeight);
    }
    m_notification.BindWindow(m_window);
    if (m_listener) { m_listener->OnModulesReady(); }
    LOG_DEBUG(Frontend, "Guest session setup: {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count());
    return true;
}

void EmulatorSession::ReleaseSession()
{
    if (m_listener) { m_listener->OnModulesReleasing(); }
    m_notification.Close();
    m_inputService.Detach();
    m_profileService.Detach();
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    m_modules.StopSession();
    m_callbackContext.reset();
    {
        std::lock_guard<std::mutex> lock(m_programEventsMutex);
        m_programEvents.clear();
    }
    m_releasePending = false;
#ifdef __APPLE__
    m_idleTrimAt = SDL_GetTicks64() + 1000;
#endif
    m_profileService.RefreshOffline();
    SettingsStore::GetInstance().SetInt(NXCoreSetting::EmulationState, static_cast<int32_t>(EmulationState::Stopped));
    m_notification.BindWindow(m_window);
    if (m_listener) { m_listener->OnModulesReady(); }
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
