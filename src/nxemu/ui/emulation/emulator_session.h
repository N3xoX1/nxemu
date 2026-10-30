#pragma once
#include <SDL.h>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-module-spec/base.h>
#include <nxemu-module-spec/system_loader.h>
#include <string>
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
        virtual void OnModulesReady(bool gameStarting) = 0;
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
    void FlushSettings();

    bool LoadGame(const std::string & path);
    void Stop();
    void TogglePause();
    bool IsRunning() const;
    bool IsLaunching() const;
    bool ReleasePending() const { return m_releasePending; }
    bool IsPaused();
    Phase CurrentPhase() const;

    void Update(bool releaseStoppedResources = true);
    void HandleKey(const SDL_KeyboardEvent & event);
    void SetViewportSize(uint32_t width, uint32_t height);

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

    struct PendingLaunch
    {
        std::string path;
        uint64_t programId = 0;
        int32_t programIndex = 0;
        int32_t previousProgramIndex = -1;
        ApplicationLaunchType launchType = ApplicationLaunchType::FrontendInitiated;
        UserChannel userChannel;
        std::chrono::steady_clock::time_point started;
    };

    bool LoadProgram(const std::string & path, int32_t programIndex, int32_t previousProgramIndex,
                     ApplicationLaunchType launchType, const UserChannel & userChannel);
    void FinishLaunch();
    void AbortLaunch(const std::string & message);
    std::string LoadFailureMessage(const std::string & path) const;
    void ReleaseModules();
    bool ResetModules();
    void RegisterSystemCallbacks();
    void ProcessProgramEvents();
    void ReportError(const std::string & message);

    static bool PrepareLaunch(void * userData);
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
    PendingLaunch m_pendingLaunch;
    bool m_launchCancelled = false;
    uint64_t m_moduleGeneration = 0;
    int32_t m_currentProgramIndex = 0;
    int32_t m_previousProgramIndex = -1;
    uint32_t m_viewportWidth = 0;
    uint32_t m_viewportHeight = 0;
    bool m_releasePending = false;
#ifdef __APPLE__
    uint64_t m_idleTrimAt = 0;
#endif
};
