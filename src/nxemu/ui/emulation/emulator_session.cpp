#include "emulator_session.h"
#include <nxemu-core/settings/identifiers.h>
#include <nxemu-core/settings/settings.h>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu/ui/host/ui_notification.h>

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

bool EmulatorSession::IsRunning() const
{
    return m_modules.IsValid() && SettingsStore::GetInstance().GetBool(NXCoreSetting::EmulationRunning);
}

void EmulatorSession::RegisterSystemCallbacks()
{
    m_callbackContext = std::make_unique<CallbackContext>(CallbackContext{this, ++m_moduleGeneration});
    IOperatingSystem & os = m_modules.Modules().OperatingSystem();
    os.RegisterExecuteProgramCallback(ExecuteProgramCallback, m_callbackContext.get());
    os.RegisterExitCallback(ExitProgramCallback, m_callbackContext.get());
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
