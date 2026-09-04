#pragma once
#include "applets/profile_select.h"
#include "applets/web_browser.h"
#include "startup_checks.h"
#include "user_interface/discord_presence.h"
#include "user_interface/widgets/rom_browser.h"
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <nxemu-core/modules/system_modules.h>
#include <nxemu-module-spec/base.h>
#include <nxemu-module-spec/system_loader.h>
#include <sciter_element.h>
#include <sciter_handler.h>
#include <sciter_ui.h>
#include <widgets/menubar.h>

#ifdef WIN32
struct Win32FullscreenState;
#endif

class SystemConfig;
class GameConfig;
class InputConfig;
class AboutDialog;

class SciterMainWindow :
    public IWindowDestroySink,
    public IWindowCloseSink,
    public IMenuBarSink,
    public IRenderWindow,
    public IKeySink,
    public IResizeSink,
    public IClickSink,
    public IStateChangeSink,
    public ITimerSink,
    public IEventSink
{
    enum class GuiAction : int32_t
    {
        Invalid,
        LoadFile,
        ExitApplication,
        PauseOrContinueEmulation,
        StopEmulation,
        OpenControllersDialog,
        OpenSystemConfiguration,
        InstallFirmwareFromFile,
        InstallFirmwareFromFolder,
        ToggleFullscreen,
        ToggleStartGamesInFullscreen,
        ToggleStartGamesWithUiHidden,
        ToggleHideUi,
        ToggleDockedMode,
        ToggleSpeedLimit,
        ResetWindowSize720p,
        ResetWindowSize900p,
        ResetWindowSize1080p,
        OpenAboutDialog,
        OpenDiscord,
        OpenReportIssues,
        OpenAppDirectory,
        OpenLogDirectory,
        RecentFileMenuFirst,
        RecentFileMenuLast = RecentFileMenuFirst + 20,
    };

    enum class Panel
    {
        RomBrowser,
        Loading,
        Pause,
        Renderer
    };

    enum
    {
        TIMER_UPDATE_UI = 5000,
        TIMER_UPDATE_INPUT,
        TIMER_UPDATE_STATUS,
        TIMER_UPDATE_INSTALL_FIRMWARE,
        TIMER_OPEN_GAME_CONFIG,
        TIMER_DEFERRED_STOP_GAME,
        TIMER_DEFERRED_FILE_EXIT,
    };

public:
    SciterMainWindow(ISciterUI & sciterUI, const char * windowTitle);
    ~SciterMainWindow();

    void ResetMenu();
    bool Show();
    void ShowConfig(const char * startPage);
    void ShowGameConfig(const char * gamePath);
    void LoadGame(const char * path, int32_t program_index = 0, ApplicationLaunchType launch_type = ApplicationLaunchType::FrontendInitiated);
    void OpenGameSaveDataLocation(const char * gamePath);
    void OpenGameModDataLocation(const char * gamePath);
    void OpenSaveDataFolderForUser(uint64_t programId, const uint8_t uuidBytes[HOST_PROFILE_UUID_SIZE]);
    static void OnSaveDataProfileSelected(void * user_data, bool has_uuid, const uint8_t uuid_bytes[16]);

    // IRenderWindow
    void * RenderSurface() const override;
    float PixelRatio() const override; 

private:
    SciterMainWindow() = delete;
    SciterMainWindow(const SciterMainWindow &) = delete;
    SciterMainWindow & operator=(const SciterMainWindow &) = delete;

    void CreateRenderWindow();
    void SetCaption(const std::string & caption);
    static void EmulationRunning(const char * setting, void * userData);
    static void EmulationStateChanged(const char * setting, void * userData);
    static void GameFileChanged(const char * setting, void * userData);
    static void GameNameChanged(const char * setting, void * userData);
    static void DisplayedFramesChanged(const char * setting, void * userData);
    static void DiskCacheLoadChanged(const char * setting, void * userData);
    static void FirmwareInstallTotalChanged(const char * setting, void * userData);
    static void HotKeysChanged(const char * setting, void * userData);
    void UpdateStatusWidgets();
    void UpdateInputDrivers();
    void PreventOSSleep();
    void AllowOSSleep();

    void OnOpenFile();
    void OnFileExit();
    void DoFileExit();
    bool ConfirmCloseEmulator();
    void OnStopGame();
    void DoStopGame();
    void OnPauseContinueGame();
    void OnExecuteProgram(uint64_t program_index);
    void OnReloadProgram();
    void OnExitProgram();
    static void ExecuteProgramCallbackThunk(size_t program_index, void * userData);
    static void ExitCallbackThunk(void * userData);
    static void CollectUserChannelEntry(const uint8_t * data, uint32_t size, void * userData);
    void OnSystemConfig();
    void OnInputConfig();
    void OnInstallFirmwareFromFile();
    void OnInstallFirmwareFromFolder();
    void BeginFirmwareInstall(const char * utf8_path);
    void StartFirmwareInstallUi();
    void StopFirmwareInstallUi();
    void RefreshFirmwareInstallLoading();
    void FinishFirmwareInstall();
    void OnRecetGame(uint32_t fileIndex);
    void OnToggleDockedMode();
    void OnToggleSpeedLimit();
    void OnToggleStartGamesInFullscreen();
    void OnToggleStartGamesWithUiHidden();
    void OnAbout();
    void OnOpenDiscord();
    void OnOpenReportIssues();
    void OnOpenAppDirectory();
    void OnOpenLogDirectory();
    void UpdateEmulationStatusText();
    const MenuBarAccelerator * HotkeyAccelerator(const char * name);
    const char * IsMenuBarAccelerator(uint32_t keyCode, uint32_t keyboardState);
    GuiAction HotkeyToGuiAction(const char * hotkeyId);
    void OnGuiAction(GuiAction action);
    const char * MenuIconResource(GuiAction action) const;
    const std::string * MenuIconSvg(GuiAction action);
    const std::string * MenuIconSvgForResource(const char * resource);

    void ToggleHideUi();
    void UpdateUIVisibility();

#ifdef WIN32
    void ToggleFullscreen();
    void EnterFullscreen();
    void ExitFullscreen();
    void ResetWindowSize(uint32_t nominal_width, uint32_t nominal_height);
#endif
    void LayoutRenderWindow();
    void UpdateMouseCursorHiding();
    void ResetMouseCursorHiding();
    void UpdateDiscordPresence();
    void UpdateLoadingScreenDetails();
    void ShowPanel(Panel panel);
    void RefreshDiskCacheLoadingText();
    void RegisterApplets();
    void RegisterSystemCallbacks();

    // IWindowDestroySink
    void OnWindowDestroy(HWINDOW hWnd) override;

    // IWindowCloseSink
    bool OnWindowCloseRequest(HWINDOW hWnd);

    // IMenuBarSink
    void OnMenuItem(int32_t id, SCITER_ELEMENT item) override;

    // IKeySink
    bool OnKeyDown(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState) override;
    bool OnKeyUp(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState) override;
    bool OnKeyChar(SCITER_ELEMENT element, SCITER_ELEMENT item, SciterKeys keyCode, uint32_t keyboardState) override;

    // IResizeSink
    bool OnSizeChanged(SCITER_ELEMENT elem) override;

    // IClickSink
    bool OnClick(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t reason) override;

    // IStateChangeSink
    bool OnStateChange(SCITER_ELEMENT elem, uint32_t eventReason, void* data) override;

    // ITimerSink    
    bool OnTimer(SCITER_ELEMENT Element, uint32_t* TimerId) override;

    // IEventSink
    bool OnEvent(SCITER_ELEMENT element, SCITER_ELEMENT source, uint32_t event_code, uint64_t reason);

    static void SettingChanged(const char* setting, void* userData);

    ISciterUI & m_sciterUI;
    ISciterWindow * m_window;
    SciterElement m_rootElement;
    SystemModules m_modules;
    std::vector<VkDeviceRecord> m_vkDeviceRecords;
    std::shared_ptr<IMenuBar> m_menuBar;
    std::shared_ptr<IRomBrowser> m_romBrowser;
    void * m_renderWindow;
    std::string m_windowTitle;
    std::unique_ptr<SystemConfig> m_systemConfig;
    std::unique_ptr<GameConfig> m_gameConfig;
    std::string m_pendingGameConfigPath;
    uint64_t m_pendingSaveDataProgramId;
    std::unique_ptr<InputConfig> m_inputConfig;
    std::unique_ptr<AboutDialog> m_aboutDialog;
    ProfileSelectApplet m_ProfileSelect;
    WebBrowserApplet m_WebBrowser;
    float m_resolutionUpFactor;
    bool m_useMultiCore;
    bool m_useSpeedLimit;
    uint32_t m_speedLimit;
    bool m_emulationRunning;
    bool m_reloadingGame;
    bool m_pendingStartInFullscreen;
    bool m_pendingStartWithUiHidden;
    std::map<std::string, std::string> m_menuIconSvgs;
    bool m_hideUi;
    uint64_t m_lastDiskCacheStatusPostMs;
    int m_lastPostedDiskCacheStage;
    bool m_shownFirstFrame;
#ifdef WIN32
    std::unique_ptr<Win32FullscreenState> m_win32Fullscreen;
#endif
    bool m_firmwareInstallInProgress;
    bool m_firmwareInstallUiActive;
    int32_t m_firmwareInstallLastTotal;
    std::thread m_firmwareInstallThread;
    bool m_mouseCursorHidden;
    uint64_t m_lastMouseActivityTick;
    int32_t m_lastTrackedMouseX;
    int32_t m_lastTrackedMouseY;
    DiscordPresence m_discordPresence;
    std::deque<std::vector<uint8_t>> m_pendingUserChannel;
    std::string m_pendingReloadPath;
    int32_t m_currentProgramIndex;
    int32_t m_previousProgramIndex;
    int32_t m_pendingReloadProgramIndex;
    ApplicationLaunchType m_pendingReloadLaunchType;
};
