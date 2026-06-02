#pragma once
#include "applets/web_browser.h"
#include "startup_checks.h"
#include "user_interface/widgets/rom_browser.h"
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

#ifdef _WIN32
struct Win32FullscreenState;
#endif

class SystemConfig;
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
        ToggleHideUi,
        ToggleDockedMode,
        ResetWindowSize720p,
        ResetWindowSize900p,
        ResetWindowSize1080p,
        OpenAboutDialog,
        OpenDiscord,
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
    };

public:
    SciterMainWindow(ISciterUI & sciterUI, const char * windowTitle);
    ~SciterMainWindow();

    void ResetMenu();
    bool Show();
    void ShowConfig(const char * startPage);
    void LoadGame(const char * path);

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
    bool ConfirmCloseEmulator();
    void OnStopGame();
    void OnPauseContinueGame();
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
    void OnToggleStartGamesInFullscreen();
    void OnAbout();
    void OnOpenDiscord();
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

#ifdef _WIN32
    void ToggleFullscreen();
    void EnterFullscreen();
    void ExitFullscreen();
    void ResetWindowSize(uint32_t nominal_width, uint32_t nominal_height);
#endif
    void LayoutRenderWindow();
    void UpdateLoadingScreenDetails();
    void ShowPanel(Panel panel);
    void RefreshDiskCacheLoadingText();
    void RegisterApplets();

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
    std::unique_ptr<InputConfig> m_inputConfig;
    std::unique_ptr<AboutDialog> m_aboutDialog;
    WebBrowserApplet m_WebBrowser;
    float m_resolutionUpFactor;
    bool m_useMultiCore;
    bool m_useSpeedLimit;
    uint32_t m_speedLimit;
    bool m_emulationRunning;
    bool m_pendingStartInFullscreen;
    std::map<std::string, std::string> m_menuIconSvgs;
    bool m_hideUi;
    uint64_t m_lastDiskCacheStatusPostMs;
    int m_lastPostedDiskCacheStage;
    bool m_shownFirstFrame;
    std::unique_ptr<Win32FullscreenState> m_win32Fullscreen;
    bool m_firmwareInstallInProgress;
    bool m_firmwareInstallUiActive;
    std::thread m_firmwareInstallThread;
};
