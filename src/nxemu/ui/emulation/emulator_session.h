#pragma once
#include <SDL.h>
#include <cstdint>
#include <deque>
#include <mutex>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-module-spec/base.h>
#include <vector>

class UiNotification;

class EmulatorSession final
{
public:
    enum class Phase
    {
        Idle,
        Loading,
        Running,
        Paused,
    };

    class Listener
    {
    public:
        virtual void OnModulesReleasing() = 0;
        virtual void OnModulesReady() = 0;
        virtual void OnGameStarted(const std::string & path) = 0;
        virtual void OnSessionError(const std::string & message) = 0;

    protected:
        ~Listener() = default;
    };

    EmulatorSession(IRenderWindow & renderWindow, UiNotification & notification, SDL_Window * window);
    ~EmulatorSession();

    void SetListener(Listener * listener) { m_listener = listener; }

    bool Initialise();
    bool IsValid() const { return m_modules.IsValid(); }
    ISystemModules & Modules() { return m_modules.Modules(); }
    bool IsRunning() const;
    void FlushSettings();
    void Update();
    bool IsPaused();
    Phase CurrentPhase() const;

private:
    EmulatorSession(const EmulatorSession &) = delete;
    EmulatorSession & operator=(const EmulatorSession &) = delete;

    using UserChannel = std::vector<std::vector<uint8_t>>;

    struct ProgramEvent
    {
        enum class Type
        {
            Execute,
            Exit,
        };
        Type type;
        uint64_t generation;
        uint32_t index;
        UserChannel userChannel;
    };

    struct CallbackContext
    {
        EmulatorSession * session;
        uint64_t generation;
    };

    void RegisterSystemCallbacks();

    static void CollectUserChannel(const uint8_t * data, uint32_t size, void * userData);
    static void ExecuteProgramCallback(uint32_t index, void * userData);
    static void ExitProgramCallback(void * userData);

    IRenderWindow & m_renderWindow;
    UiNotification & m_notification;
    SDL_Window * m_window;
    Listener * m_listener = nullptr;
    SystemModules m_modules;
    std::mutex m_programEventsMutex;
    std::deque<ProgramEvent> m_programEvents;
    std::unique_ptr<CallbackContext> m_callbackContext;
    uint64_t m_moduleGeneration = 0;
};
