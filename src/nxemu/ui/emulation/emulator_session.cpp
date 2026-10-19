#include "emulator_session.h"
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
    if (!IsRunning())
    {
        return;
    }
    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    os.SetEmulationPaused(!os.IsEmulationPaused());
}

bool EmulatorSession::IsRunning() const
{
    return m_modules.IsValid() && SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning);
}

bool EmulatorSession::IsPaused()
{
    return IsRunning() && m_modules.Modules().OperatingSystem().IsEmulationPaused();
}

EmulatorSession::Phase EmulatorSession::CurrentPhase() const
{
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
    ProcessProgramEvents();
    if (releaseStoppedResources && m_releasePending && !IsRunning() &&
        SettingsStore::GetInstance().GetInt(NXCoreSetting::EmulationState) == static_cast<int32_t>(EmulationState::Stopped))
    {
        if (!ResetModules())
        {
            ReportError(Tr("Required modules failed to reload after stopping the game."));
            return;
        }
        SettingsStore::GetInstance().SetInt(NXCoreSetting::EmulationState, static_cast<int32_t>(EmulationState::Stopped));
    }
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
    std::error_code error;
    if (!std::filesystem::is_regular_file(std::filesystem::path{Common::FS::ToU8String(path)}, error))
    {
        ReportError(Tr("The game file does not exist:\n%1", {path}));
        return false;
    }
    uint64_t programId = 0;
    if (m_modules.IsValid())
    {
        IRomInfoPtr info(m_modules.Modules().Systemloader().RomInfo(path.c_str(), 0, programIndex));
        if (info)
        {
            info->ReadProgramId(programId);
        }
    }
    if (!ResetModules())
    {
        ReportError(Tr("Required modules failed to load."));
        return false;
    }

    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    m_releasePending = true;
    for (const std::vector<uint8_t> & entry : userChannel)
    {
        os.PushUserChannelEntry(entry.data(), static_cast<uint32_t>(entry.size()));
    }
    if (!m_modules.Modules().Systemloader().LoadRom(path.c_str(), programId, programIndex, previousProgramIndex, launchType))
    {
        ReportError(Tr("Could not load the game:\n%1", {path}));
        return false;
    }
    m_currentProgramIndex = programIndex;
    m_previousProgramIndex = previousProgramIndex;
    if (m_listener != nullptr)
    {
        m_listener->OnGameStarted(path);
    }
    return true;
}

bool EmulatorSession::ResetModules()
{
    m_releasePending = false;
    if (m_listener != nullptr)
    {
        m_listener->OnModulesReleasing();
    }
    m_notification.Close();
    SettingsStore::GetInstance().SetBool(NXCoreSetting::EmulationRunning, false);
    m_modules.ShutDown();
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
        m_listener->OnModulesReady();
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
