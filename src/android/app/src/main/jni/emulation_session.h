#pragma once

#include <array>
#include <deque>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nxemu-core/modules/system_modules.h>
#include <nxemu-module-spec/operating_system.h>
#include <nxemu-module-spec/system_loader.h>
#include "android_render_window.h"

struct ANativeWindow;

class EmulationSession
{
public:
    static EmulationSession & GetInstance();

    void InitializeSystem();
    void ShutdownSystem();

    std::string QueryRomMetadata(const std::string & path);
    bool ScanGameFiles(const std::vector<GameFileEntry> & files, IGameListScanCallback & callback);
    static std::string RomTitle(IRomInfo * rom);
    static std::vector<uint8_t> RomIcon(IRomInfo * rom);

    struct GameAddOn
    {
        enum class Kind
        {
            Update,
            AddOnContent,
            Mod,
        };
        Kind kind = Kind::Mod;
        std::string key;
        std::string name;
        std::string version;
        bool enabled = false;
    };

    struct GameProperties
    {
        bool readable = false;
        uint64_t programId = 0;
        std::string version;
        std::string developer;
        std::string modDirectory;
        std::vector<GameAddOn> addOns;
        std::string saveDirectory;
        std::string saveImportDirectory;
        uint64_t saveSize = 0;
        std::string shaderDirectory;
        uint64_t shaderSize = 0;
    };

    GameProperties QueryGameProperties(const std::string & path, uint64_t program_id);
    void SetDisabledAddOns(uint64_t program_id, const std::vector<std::string> & names);
    std::string QueryRomInfo(const std::string & path, uint64_t program_id);

    std::array<double, 4> GetPerfStats();
    uint32_t GetShadersBuilding();
    std::string GetFirmwareVersion();
    std::string GetGameCardFirmwareUpdate(const std::string & path);
    FirmwareInstallStatus InstallFirmware(const std::string & path, IFirmwareInstallObserver & observer);

    void SetOverlayButton(int port, int button_id, bool pressed);
    void SetOverlayJoystick(int port, int stick_id, float x, float y);
    int GetStyleIndex(int player_index);

    enum class ProgramEventResult
    {
        None,
        Reloaded,
        Exited,
        Failed,
    };

    bool Run(ANativeWindow * native_window, float pixel_ratio, const std::string & rom_path, uint64_t program_id);
    ProgramEventResult ProcessProgramEvents();
    void Stop();
    void SetPaused(bool paused);
    bool IsPaused();
    void SetNativeWindow(ANativeWindow * native_window);
    void SurfaceDestroyed();
    void SurfaceChanged();
    bool ReadAppletCapture(std::vector<uint8_t> & out, uint32_t & width, uint32_t & height);
    void AppletCaptureSize(uint32_t & width, uint32_t & height);

    ANativeWindow * NativeWindow() const;
    float PixelRatio() const;

    IOperatingSystem * OperatingSystem();
    SystemModules * Modules();

private:
    EmulationSession() = default;

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
        EmulationSession * session;
        IOperatingSystem * os;
        uint64_t generation;
    };

    void RestoreStubModulesLocked();
    bool StartProgramLocked(uint64_t program_id, int32_t program_index, int32_t previous_program_index, ApplicationLaunchType launch_type,
                            const UserChannel & user_channel);
    void QueueProgramEvent(ProgramEvent event);

    static void CollectUserChannel(const uint8_t * data, uint32_t size, void * userData);
    static void ExecuteProgramCallback(uint32_t index, void * userData);
    static void ExitProgramCallback(void * userData);

    mutable std::mutex m_mutex;
    AndroidRenderWindow m_render_window;
    std::unique_ptr<SystemModules> m_system_modules;
    ANativeWindow * m_native_window = nullptr;
    float m_pixel_ratio = 1.0f;

    // Programs in a multi-program title (e.g. a collection's menu and its games) ask the frontend to load
    // another program from the same file, or to go back when they exit. See EmulatorSession on desktop.
    std::string m_rom_path;
    uint64_t m_program_id = 0;
    int32_t m_current_program_index = 0;
    int32_t m_previous_program_index = -1;
    std::unique_ptr<CallbackContext> m_callback_context;
    uint64_t m_module_generation = 0;
    std::mutex m_program_events_mutex;
    std::deque<ProgramEvent> m_program_events;
};
