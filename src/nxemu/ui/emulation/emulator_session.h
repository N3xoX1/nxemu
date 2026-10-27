#pragma once
#include <SDL.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu/ui/host/profile_service.h>
#include <nxemu/ui/host/input_service.h>
#include <nxemu-module-spec/base.h>
#include <nxemu-module-spec/system_loader.h>
#include <string>
#include <thread>
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
    bool InitialiseDisplay();
    // Host stays usable when no guest is running.
    bool FrontendReady() const { return m_modules.FrontendValid(); }
    bool HasGuest() const { return m_modules.IsValid(); }
    ISystemloader & Loader() { return m_modules.Loader(); }
    IVideo & DisplayVideo() { return m_modules.DisplayVideo(); }
    ISystemModules & Modules() { return m_modules.Modules(); }
    void FlushSettings();
    HostProfileService & Profiles();
    HostInputService & Inputs() noexcept { return m_inputService; }

    // Temporary OS for host-only operations that still require guest services.
    bool EnsureGuest() { return HasGuest() || PrepareSession(); }
    void ReleaseIdleGuest() { if (!IsRunning() && !m_releasePending && HasGuest()) ReleaseSession(); }

    bool LoadGame(const std::string & path);
    void Stop();
    void TogglePause();
    // True from the launch request on, including while the game is still loading.
    bool IsRunning() const;
    // True once emulation has started, so the guest OS can be queried.
    bool IsEmulating() const;
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

    // A game launched from the UI, read on m_loadThread then started by FinishLoad.
    struct PendingLaunch
    {
        std::string path;
        int32_t programIndex = 0;
        int32_t previousProgramIndex = -1;
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point resetDone;
        bool cancelled = false;
    };

    bool LoadProgram(const std::string & path, int32_t programIndex, int32_t previousProgramIndex,
                     ApplicationLaunchType launchType, const UserChannel & userChannel);
    void FinishLoad();
    void ReportLoadFailure(const std::string & path);
    bool PrepareSession();
    void ReleaseSession();
    void RegisterSystemCallbacks();
    void ProcessProgramEvents();
    void ReportError(const std::string & message);

    static void CollectUserChannel(const uint8_t * data, uint32_t size, void * userData);
    static void ExecuteProgramCallback(uint32_t index, void * userData);
    static void ExitProgramCallback(void * userData);

    IRenderWindow & m_renderWindow;
    UiNotification & m_notification;
    SDL_Window * m_window;
    Listener * m_listener = nullptr;
    SystemModules m_modules;
    HostProfileService m_profileService;
    HostInputService m_inputService;
    std::mutex m_programEventsMutex;
    std::deque<ProgramEvent> m_programEvents;
    std::unique_ptr<CallbackContext> m_callbackContext;
    uint64_t m_moduleGeneration = 0;
    int32_t m_currentProgramIndex = 0;
    int32_t m_previousProgramIndex = -1;
    uint32_t m_viewportWidth = 0;
    uint32_t m_viewportHeight = 0;
    bool m_releasePending = false;
    PendingLaunch m_pendingLaunch;
    std::thread m_loadThread;
    std::atomic<bool> m_loadDone{false};
    bool m_loading = false;
    bool m_loadResult = false;
#ifdef __APPLE__
    uint64_t m_idleTrimAt = 0;
#endif
};
